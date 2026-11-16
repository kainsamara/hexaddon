#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "lwip/igmp.h"
#include "lwip/netif.h"

#include "settings.h"
#include "web.h"

static DNSServer dns;
static bool portal = false;
static bool apFailed = false;  // softAP never came up: web UI unreachable
static bool mdnsUp = false;
static bool connecting = false;
static uint32_t tConnect = 0;
static uint8_t failCount = 0;    // consecutive rejected STA attempts
static uint32_t retryAt = 0;     // deadline of a periodic STA retry from the portal
static String apSsid;

// ---------------- TX power ceiling ----------------
// WIFI_TX_POWER_DBM (settings.h) is a BOARD constraint, not a preference:
// the antenna feed on this PCB is mistuned and the radio chokes above the
// ceiling (own TX deafens the RX). The ceiling is re-asserted after every
// WiFi mode change (mode switches can re-run PHY config paths in the core)
// and audited in netLoop: if anything ever raises the limit above the
// ceiling, it is forced back and logged. The serial "TXPWR" command is
// the ONLY sanctioned exception — SWR diagnostics must sweep the FULL
// range including the top — and it latches txManual until reboot.
static float txCeilDbm = WIFI_TX_POWER_DBM;
static bool txManual = false;      // serial TXPWR override in effect
static bool txAboveLogged = false;

// map a dBm value onto the discrete wifi_power_t steps (0 = max/19.5)
static wifi_power_t txPowerLevel(float dbm) {
  if (dbm >= 19.25f) return WIFI_POWER_19_5dBm;
  if (dbm >= 18.75f) return WIFI_POWER_19dBm;
  if (dbm >= 17.75f) return WIFI_POWER_18_5dBm;
  if (dbm >= 16) return WIFI_POWER_17dBm;
  if (dbm >= 14) return WIFI_POWER_15dBm;
  if (dbm >= 12) return WIFI_POWER_13dBm;
  if (dbm >= 10) return WIFI_POWER_11dBm;
  if (dbm >= 8) return WIFI_POWER_8_5dBm;
  if (dbm >= 6) return WIFI_POWER_7dBm;
  if (dbm >= 3.5f) return WIFI_POWER_5dBm;
  if (dbm >= 1) return WIFI_POWER_2dBm;
  return WIFI_POWER_MINUS_1dBm;
}

// force the board ceiling back (no-op while a manual TXPWR override runs)
static void txApplyCeiling() {
  if (txManual) return;
  wifi_power_t lvl = txPowerLevel(txCeilDbm);
  if (WiFi.getTxPower() != lvl) {
    if (!WiFi.setTxPower(lvl))
      Serial.println(F("[net] tx ceiling re-assert FAILED"));
  }
}


// ---- mDNS liveness watchdog -----------------------------------------------
// Hunts the "page dead by NAME, alive by IP" state. The responder can look
// perfectly up (mdnsUp) while being deaf, and the router can silently drop
// our multicast membership. So once a minute the device asks the AIR to
// resolve its own hostname over real mDNS: the query leaves as multicast,
// the AP bridges it back, our own responder must answer. No answer three
// times in a row -> the responder (or the path) is dead -> request recovery,
// executed in netLoop's context (at most once per 30 min).
// Runs on its own tiny task: the query blocks up to 750 ms and must never
// stall webLoop.
static volatile bool mdnsSelfOk = true;  // last raw-probe verdict for /api/status
static volatile uint8_t mdnsLastProbe = 0;  // 2 ours / 1 foreign / 0 no answer

// hostname snapshot for the watchdog task. settings.hostname is a String
// owned by the loop context: applyConfig() may reassign it (free+realloc)
// exactly while the task reads c_str() — a use-after-free race. The copy
// lives in a plain char buffer behind a mutex, refreshed by netLoop.
static SemaphoreHandle_t wdHostMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}
static char wdHost[33] = "hexaddon";
static void wdHostUpdate() {
  static String last;
  if (last == settings.hostname) return;
  last = settings.hostname;
  xSemaphoreTake(wdHostMutex(), portMAX_DELAY);
  strncpy(wdHost, settings.hostname.c_str(), sizeof(wdHost) - 1);
  wdHost[sizeof(wdHost) - 1] = 0;
  xSemaphoreGive(wdHostMutex());
}

// ---- mDNS / IGMP membership -----------------------------------------------
// Re-assert the IGMP membership for the mDNS group 224.0.0.251.
//
// WHY: routers with IGMP snooping (practically every home router) forget a
// port's group membership after a few minutes without membership reports
// and then stop delivering INBOUND multicast to it — the responder stays
// alive but never sees a query: ".local dead, IP alive". The old firmware
// never hit this: the hidden-AP connect dropped constantly, and every
// re-association refreshed the membership for free. tryDirectedConnect()
// made the link STABLE — and stability is what starves the snooping table.
// The fix is to refresh the membership ourselves: a leave+join pair emits
// a fresh IGMP report WITHOUT touching TCP or the Wi-Fi link.
// is 224.0.0.251 actually in lwip's IGMP group table for our netif?
// Proves/disproves that mdnsRejoin() really re-asserted the membership
// (IDF's lwip keeps the group list per-netif via netif client data)
static bool mdnsGroupPresent() {
#if LWIP_IGMP
  struct netif *n = netif_default;
  if (!n) return false;
  ip4_addr_t grp;
  IP4_ADDR(&grp, 224, 0, 0, 251);
  return igmp_lookfor_group(n, &grp) != nullptr;
#else
  return false;
#endif
}

static bool mdnsRejoin() {
  struct netif *n = netif_default;  // the STA — the only interface with a route
  if (!n || !netif_is_up(n)) return false;
  ip4_addr_t grp;
  IP4_ADDR(&grp, 224, 0, 0, 251);  // mDNS multicast group
  igmp_leavegroup(netif_ip4_addr(n), &grp);  // no-op (and fine) if not a member
  return igmp_joingroup(netif_ip4_addr(n), &grp) == ERR_OK;
}

// ---- raw legacy-unicast mDNS self-probe -------------------------------
// The IDF mdns_query_a() API returns ESP_ERR_INVALID_STATE ("mDNS is not
// running") on this build even while the responder is demonstrably alive
// and resolving — a proven liar, useless as a health signal. Instead send
// a RAW legacy query (RFC 6762 §5.1): a query from a source port OTHER
// than 5353 MUST be answered UNICAST to that port — the reply then needs
// no group membership to be heard and exercises the responder's full RX
// path. This is the official Chrome/Bonjour discovery flow, supported by
// every responder including ours.
//
// RETURN: 2 = answered with OUR ip, 1 = answered with ANOTHER ip (name
// conflict!), 0 = no answer. NO ANSWER IS NOT PROOF OF DEATH (some
// stacks/proxies answer multicast-only) — it is telemetry, never a
// recovery trigger: a lying health check driving an automated "fix" is
// how this firmware briefly greeted a healthy network with goodbye
// packets every 90 seconds.
static uint8_t mdnsRawProbe(const char *host, IPAddress &who) {
  who = IPAddress(0, 0, 0, 0);
  WiFiUDP udp;
  if (!udp.begin(0)) return 0;  // ephemeral port => legacy query semantics
  uint8_t q[64];
  int o = 0;
  q[o++] = 0; q[o++] = 0;        // transaction id
  q[o++] = 0; q[o++] = 0;        // standard query
  q[o++] = 0; q[o++] = 1;        // QDCOUNT = 1
  q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0;  // AN/NS/AR = 0
  int hl = strlen(host);
  if (hl < 1 || hl > 24) { udp.stop(); return 0; }
  q[o++] = (uint8_t)hl;
  memcpy(q + o, host, hl); o += hl;
  q[o++] = 5; memcpy(q + o, "local", 5); o += 5;
  q[o++] = 0;
  q[o++] = 0; q[o++] = 1;        // QTYPE = A
  q[o++] = 0x80; q[o++] = 0x01;  // QCLASS = IN | unicast-response OK
  udp.beginPacket(IPAddress(224, 0, 0, 251), 5353);
  udp.write(q, o);
  if (!udp.endPacket()) { udp.stop(); return 0; }

  uint8_t res = 0;
  uint8_t buf[512];
  uint32_t t0 = millis();
  while (millis() - t0 < 750) {
    int p = udp.parsePacket();
    if (!p) { delay(5); continue; }
    if (udp.remotePort() != 5353) continue;  // not an mDNS answer
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n < 12) continue;
    // scan the packet for an A record trailer: 00 01 (A) xx xx (class,
    // with or without the cache-flush bit) .... .... .. 00 04 <rdlen=4>
    // then 4 bytes of RDATA = the address
    res = 2;  // an answer arrived from port 5353 — ours unless proven below
    for (int i = 0; i + 13 < n; i++) {
      if (buf[i] == 0 && buf[i + 1] == 1 &&              // TYPE = A
          (buf[i + 2] == 0x80 || buf[i + 2] == 0) &&     // class IN ± cache-flush
          buf[i + 3] == 1 && buf[i + 8] == 0 && buf[i + 9] == 4) {  // rdlen = 4
        IPAddress b(buf[i + 10], buf[i + 11], buf[i + 12], buf[i + 13]);
        who = b;
        if (b != WiFi.localIP()) res = 1;  // someone else owns our name
        break;
      }
    }
    break;  // first answer is enough
  }
  udp.stop();
  return res;
}

// ---- fallback mini-responder -------------------------------------------
// Field evidence: the IDF stack can announce (the router proxies our name)
// yet never answer A queries — half-dead. A dead stack has released the
// UDP :5353 port (mdns_free closes its sockets); a live one holds it. So
// after a probe with no answer we try to GRAB :5353 ourselves: if the bind
// succeeds, the IDF stack is dead and this mini-responder takes over .local
// completely; if it fails, the stack is alive and we back off silently.
// It answers A/ANY queries for our own name and nothing else.
static WiFiUDP miniRes;
static bool miniArmed = false;

static void miniTryArm() {
  if (miniArmed) return;
  static uint32_t retryAt = 0;
  if ((int32_t)(millis() - retryAt) >= 0) retryAt = 0;  // throttle window open
  if (retryAt) return;
  retryAt = millis() + 1800000UL;  // one attempt per 30 min: a refused bind
                                   // logs an ERROR line from the core
  if (!miniRes.beginMulticast(IPAddress(224, 0, 0, 251), 5353)) {
    // bind refused: the IDF stack is alive and holding :5353. With a
    // Keenetic-style mDNS reflector on the router our probes can NEVER
    // see our own answers (the reflector does not loop packets back into
    // the source segment), so ans=0 alone proves nothing — this bind
    // result is the ONLY reliable liveness signal for the IDF stack.
    static bool heldLogged = false;
    if (!heldLogged) {
      heldLogged = true;
      Serial.println(F("[net] mDNS: :5353 held by the IDF stack (alive); "
                       "probe blindness is expected behind an mDNS reflector"));
    }
    return;
  }
  miniArmed = true;
  Serial.println(F("[net] mDNS: IDF stack is not holding :5353 -> mini-responder ARMED"));
}

// answer one A query for "<host>.local" if that is what arrived
static void miniService(const char *host) {
  int p = miniRes.parsePacket();
  if (p < 17) return;
  uint8_t in[512];
  int n = miniRes.read(in, sizeof(in));
  if (n < 17) return;
  if (in[2] & 0x80) return;                  // not a query
  if (((in[4] << 8) | in[5]) < 1) return;    // no questions
  // parse the first qname (plain labels; compressed query names are bogus)
  char name[64];
  int ln = 0, o = 12;
  while (o < n) {
    uint8_t l = in[o++];
    if (l == 0) break;
    if (l & 0xC0 || l > 32) return;
    if (ln && ln + 1 < (int)sizeof(name)) name[ln++] = '.';
    for (int i = 0; i < l && o < n; i++, o++)
      if (ln + 1 < (int)sizeof(name)) name[ln++] = (char)tolower(in[o]);
  }
  if (o + 4 > n) return;
  name[ln] = 0;
  char want[48];
  snprintf(want, sizeof(want), "%s.local", host);
  if (strcasecmp(name, want) != 0) return;
  uint16_t qtype = (uint16_t)((in[o] << 8) | in[o + 1]);
  if (qtype != 1 && qtype != 255) return;    // A or ANY only

  uint8_t out[80];
  memcpy(out, in, 2);                        // echo the transaction id
  out[2] = 0x84; out[3] = 0x00;              // QR | AA
  out[4] = 0; out[5] = 1;                    // QDCOUNT (echo the question)
  out[6] = 0; out[7] = 1;                    // ANCOUNT
  out[8] = 0; out[9] = 0; out[10] = 0; out[11] = 0;
  int qend = o + 4;                          // qname + type + class
  int qlen = qend - 12;
  memcpy(out + 12, in + 12, qlen);
  int w = 12 + qlen;
  out[w++] = 0xC0; out[w++] = 0x0C;          // name = pointer to qname
  out[w++] = 0; out[w++] = 1;                // TYPE = A
  out[w++] = 0x80; out[w++] = 0x01;          // CLASS = IN | cache-flush
  out[w++] = 0; out[w++] = 0; out[w++] = 0; out[w++] = 120;  // TTL 120 s
  out[w++] = 0; out[w++] = 4;                // RDLENGTH = 4
  IPAddress ip = WiFi.localIP();
  out[w++] = ip[0]; out[w++] = ip[1]; out[w++] = ip[2]; out[w++] = ip[3];
  // legacy query (source port != 5353) is answered UNICAST, RFC 6762 §5.1
  if (miniRes.remotePort() == 5353) miniRes.beginMulticastPacket();
  else miniRes.beginPacket(miniRes.remoteIP(), miniRes.remotePort());
  miniRes.write(out, w);
  miniRes.endPacket();
}

static void mdnsWatchdogTask(void *) {
  uint8_t prev = 0xFF;  // last probe verdict, for change-only logging
  uint32_t probeAt = 0;
  for (;;) {
    delay(100);
    if (mdnsUp && WiFi.status() == WL_CONNECTED && miniArmed) {
      char host[33];
      xSemaphoreTake(wdHostMutex(), portMAX_DELAY);
      strcpy(host, wdHost);
      xSemaphoreGive(wdHostMutex());
      miniService(host);
    }
    if ((int32_t)(millis() - probeAt) >= 0) {
      probeAt = millis() + 60000;
      if (!mdnsUp || WiFi.status() != WL_CONNECTED) {
        prev = 0xFF;  // re-log the first verdict after a reconnect
        continue;
      }
      if (WiFi.scanComplete() == WIFI_SCAN_RUNNING || webScanRecent()) {
        // the radio is off-channel scanning right now: mDNS deafness here
        // is physics, not a symptom
        continue;
      }
      char host[33];
      xSemaphoreTake(wdHostMutex(), portMAX_DELAY);
      strcpy(host, wdHost);
      xSemaphoreGive(wdHostMutex());
      IPAddress who;
      uint8_t v = mdnsRawProbe(host, who);
      if (v != prev) {
        prev = v;
        if (v == 2)
          Serial.println(F("[net] mDNS: legacy self-probe OK (answered with our ip)"));
        else if (v == 1)
          Serial.printf("[net] mDNS: %s.local answered by %s - ANOTHER device (name conflict?)\n",
                        host, who.toString().c_str());
        else
          Serial.println(F("[net] mDNS: legacy self-probe: NO unicast answer"));
      }
      // no answer for our own name: either the responder is dead (then
      // :5353 is free and the mini-responder arms) or we are not hearing
      // queries at all (the mini bind will fail — a live stack holds the
      // port — and we keep observing)
      if (v == 0) miniTryArm();
      // telemetry: mdnsSelfOk feeds /api/status; "no answer" alone is NOT
      // a failure, a foreign ip is. With the mini armed and answering, the
      // NEXT probe will succeed and flip the verdict back to ok.
      mdnsLastProbe = v;
      mdnsSelfOk = (v != 1);
    }
  }
}

// after STA connects with the portal still up, keep the AP alive this long:
// the phone hangs on it and polls /api/status, so the page learns the new
// STA IP and can follow the device (Android cannot resolve <host>.local)
#define PORTAL_GRACE_MS 6000
static uint32_t portalGraceAt = 0;

// the core knows exactly why STA dropped: NO_AP_FOUND (wrong SSID / 5 GHz
// only network), 4WAY_HANDSHAKE_TIMEOUT (wrong password), BEACON_TIMEOUT
// (weak signal). Keep the last reason for /api/status (the portal shows it)
// and log it instead of guessing.
static uint8_t lastDisc = 0;

static void onStaDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
  uint8_t r = info.wifi_sta_disconnected.reason;
  // our own disconnect() (portal startup / retry end) reports ASSOC_LEAVE —
  // it is not a connection failure and must not overwrite the real reason
  // (e.g. 4WAY_HANDSHAKE_TIMEOUT = wrong password)
  if (r == WIFI_REASON_ASSOC_LEAVE) return;
  lastDisc = r;
  failCount++;
  Serial.printf("[net] disconnected: %s (%u)\n",
                WiFi.disconnectReasonName((wifi_err_reason_t)r), (unsigned)r);
}

// Hidden-AP fallback. The built-in connect scan (even with
// WIFI_ALL_CHANNEL_SCAN from netBegin) is not reliable against hidden APs:
// the STA keeps reporting NO_AP_FOUND although the network is right there.
// After a few failed rounds we scan ourselves — all channels, hidden
// records included — and connect straight to channel+BSSID, no scan-based
// matching involved anymore. Two kinds of records can save us:
//   1. an exact SSID match — the AP does answer a directed probe
//   2. a blank-SSID record — a beacon-only hidden AP: some routers (e.g.
//      ZyXEL/Keenetic) ignore probe requests entirely while hidden and
//      only beacon with an empty SSID; the assoc request then carries our
//      real SSID, which the AP validates — exactly how phones connect.
//      With several hidden APs around we take the strongest and hope it
//      is ours; a mismatch just fails the assoc and we retry later.
// Blocking (~2s over all channels), called from netLoop only, throttled.
static uint32_t directedNextAt = 0;
static void tryDirectedConnect() {
  if (!settings.wifiSsid.length()) return;
  // a web-side /api/networks scan may own the radio right now: our sync
  // scan would bounce off the core's SCANNING bit and waste this attempt —
  // come back on the next netLoop pass instead (no throttle bump)
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  Serial.printf("[net] fallback scan for \"%s\" (hidden incl.)\n",
                settings.wifiSsid.c_str());
  // stop the auto-retry carousel first: it owns the radio, and a scan
  // started mid-attempt just fails
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  // hostname must ride EVERY connect: routers (ZyXEL/Keenetic) register the
  // DHCP client name in their DNS — a reconnect without it drops the
  // name->IP record and the page stops opening "by name" until a reboot
  WiFi.setHostname(settings.hostname.c_str());
  int n = WiFi.scanNetworks(false, true, false, 120);
  int named = -1, hiddenRec = -1;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s == settings.wifiSsid) {
      if (named < 0 || WiFi.RSSI(i) > WiFi.RSSI(named)) named = i;
    } else if (!s.length()) {
      if (hiddenRec < 0 || WiFi.RSSI(i) > WiFi.RSSI(hiddenRec)) hiddenRec = i;
    }
  }
  int best = named >= 0 ? named : hiddenRec;
  if (best >= 0) {
    const uint8_t *b = WiFi.BSSID(best);
    Serial.printf("[net] fallback: %s ch=%d rssi=%d bssid=%02x%02x%02x..%02x%02x%02x\n",
                  named >= 0 ? "ssid match" : "hidden beacon (blank SSID)",
                  (int)WiFi.channel(best), (int)WiFi.RSSI(best),
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str(),
               WiFi.channel(best), WiFi.BSSID(best));
    tConnect = millis();
  } else {
    // neither a named record nor a blank-SSID beacon: wrong SSID (it is
    // CASE-SENSITIVE), the AP is off, or it is 5 GHz-only
    Serial.println(F("[net] fallback: no name match and no hidden beacons"));
  }
  WiFi.scanDelete();
  WiFi.setAutoReconnect(true);
  txApplyCeiling();  // sync scan + BSSID connect path: keep the board ceiling
  directedNextAt = millis() + 30000;
}

static void startConnect() {
  WiFi.setAutoReconnect(true);  // quick retries while we just want to connect
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  tConnect = millis();
  connecting = true;
  Serial.printf("[net] connecting to \"%s\"\n", settings.wifiSsid.c_str());
}

static void startPortal() {
  // Full portal: STA off completely. Any STA activity hops radio channels
  // and blinks the AP beacon — phones refuse to show such a network.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_AP);
  String mac = WiFi.macAddress();
  apSsid = "HEXADDON-" + mac.substring(12, 14) + mac.substring(15, 17);
  // softAP can silently fail right after a mode switch — retry and report
  bool ap = false;
  for (int i = 0; i < 5 && !ap; i++) {
    ap = WiFi.softAP(apSsid.c_str());
    if (!ap) {
      Serial.printf("[net] softAP try %d failed\n", i + 1);
      delay(150);
    }
  }
  if (ap) dns.start(53, "*", WiFi.softAPIP());  // no AP -> DNS has no interface to answer on
  portal = true;
  apFailed = !ap;
  retryAt = 0;
  failCount = 0;
  tConnect = millis();
  txApplyCeiling();  // mode switch to AP: the PHY limit must survive it
  Serial.printf("[net] setup portal: \"%s\" ip=%s ap=%s ch=%d\n", apSsid.c_str(),
                WiFi.softAPIP().toString().c_str(), ap ? "up" : "FAIL",
                (int)WiFi.channel());
}

static void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // back to normal STA behavior
  txApplyCeiling();  // mode switch: the PHY limit must survive it
  portal = false;
  apSsid = "";
  Serial.println(F("[net] portal closed, STA connected"));
}

void netBegin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Hidden APs never answer broadcast probes and hide their SSID in beacons,
  // so the default WIFI_FAST_SCAN connect scan does not find them at all
  // (NO_AP_FOUND -> 10 fails -> portal, even though the AP is right there).
  // ALL_CHANNEL_SCAN sweeps every channel and directs a probe with our SSID,
  // which a hidden AP must answer. Applies to every WiFi.begin() below,
  // including the auto-reconnect after the router hides its SSID.
  // Costs ~1-2s on connect; a mains-powered device does not care.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  // modem sleep adds 100-300ms latency spikes and drops packets with
  // power-saving clients (phones!) — Chrome then retries a request until
  // it gives up with ERR_TOO_MANY_RETRIES. A mains-powered device has no
  // reason to sleep the radio.
  WiFi.setSleep(false);
  netSetTxPower(WIFI_TX_POWER_DBM);
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // NTP clock (for the Clock display mode): syncs whenever STA is up.
  // The timezone comes from settings.tz (applied in Settings::sanitize).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.printf("[net] configured=%d\n", (int)settings.wifiConfigured);
  if (settings.wifiConfigured) {
    startConnect();
  } else {
    startPortal();
  }
  // mDNS liveness watchdog (see mdnsWatchdogTask): hunts the
  // ".local dead, IP alive" state and requests a stack restart on
  // proven deafness. Small stack — it only sleeps and queries.
  xTaskCreate(mdnsWatchdogTask, "mdnsWd", 4096, nullptr, 3, nullptr);
}

void netLoop() {
  static int lastSta = -1;
  static uint32_t lastConn = 0;  // last moment STA was connected (0 = never)
  static uint32_t hbAt = 0;      // heartbeat: catch "page dead" states live
  wdHostUpdate();  // feed the watchdog's hostname snapshot (see net.h race note)

  // TX power ceiling audit (see the ceiling block on top): whatever raised
  // the PHY limit above the board ceiling — a core re-init, a mode switch
  // we forgot to cover, a future edit — gets forced back within 5 s. The
  // serial TXPWR override (txManual) is the only thing allowed above it.
  {
    static uint32_t txChkAt = 0;
    if ((int32_t)(millis() - txChkAt) >= 0) {
      txChkAt = millis() + 5000;
      if (!txManual && WiFi.getTxPower() > txPowerLevel(txCeilDbm)) {
        if (!txAboveLogged) {
          txAboveLogged = true;
          Serial.printf("[net] tx power %.2f dBm ABOVE the %.1f dBm board ceiling -> forced back\n",
                        WiFi.getTxPower() * 0.25f, txCeilDbm);
        }
        WiFi.setTxPower(txPowerLevel(txCeilDbm));
      } else {
        txAboveLogged = false;  // back at/below the ceiling: re-arm the log
      }
    }
  }

  int st = (int)WiFi.status();
  if (st != lastSta) {
    lastSta = st;
    Serial.printf("[net] sta status=%d ip=%s\n", st, WiFi.localIP().toString().c_str());
  }
  if ((int32_t)(millis() - hbAt) >= 0) {
    hbAt = millis() + 30000;
    // st=3 but the page is dead -> the stack is half-alive (reboot fixes);
    // heap dropping over hours -> a leak; mdns=0 while connected -> the
    // ".local dead, IP alive" state; grp=0 -> lwip itself lost the
    // multicast membership (snooping victim); ans = last raw legacy probe:
    // 2 answered-with-our-ip, 1 answered-by-another-device (conflict!),
    // 0 no answer -> the mini-responder then tries to take over :5353;
    // mini=1 -> it armed (the IDF stack was dead, .local is served by us).
    Serial.printf("[net] hb st=%d ip=%s heap=%u min=%u mdns=%d grp=%d ans=%d mini=%d\n", st,
                  WiFi.localIP().toString().c_str(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  mdnsUp ? 1 : 0, mdnsGroupPresent() ? 1 : 0,
                  (int)mdnsLastProbe, miniArmed ? 1 : 0);
  }
  if (portal) dns.processNextRequest();

  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn) {
    connecting = false;
    lastConn = millis();
    lastDisc = 0;  // stale reason must not surface after a success
    failCount = 0;
    {
      // DHCP may hand out a DIFFERENT lease after a reconnect (ours rides
      // the fallback path) — a silent IP change looks exactly like "the
      // page is dead" for anything still using the old address
      static IPAddress lastIp;
      if (WiFi.localIP() != lastIp) {
        lastIp = WiFi.localIP();
        Serial.printf("[net] ip now %s\n", lastIp.toString().c_str());
      }
    }
    // NOTE: the watchdog is TELEMETRY ONLY now. The official
    // mdns_query_a() health check turned out to return
    // ESP_ERR_INVALID_STATE while the responder was verifiably alive
    // (log-proven in the field), and a lying health signal driving
    // automated recovery (re-announce with its goodbye window, then the
    // mdns free/init roulette) actively HARMED a healthy network. No
    // auto-recovery, no announce timers: the raw legacy probe in the
    // watchdog logs what it sees, hb reports it, and the only medicines
    // left are the benign ones below (IGMP refresh, reconnect re-announce,
    // begin-retry) plus a plain reboot if a human decides so.

    // PROPHYLAXIS: while the link is stable, refresh the IGMP membership
    // every 3 minutes ourselves (see mdnsRejoin why stability itself
    // starves the router's snooping table). Two UDP-sized packets, no
    // TCP impact, no link impact.
    {
      static uint32_t rejoinAt = 0;
      if ((int32_t)(millis() - rejoinAt) >= 0) {
        rejoinAt = millis() + 180000UL;
        mdnsRejoin();
      }
    }

    {
      // reconnected while the mDNS stack stayed up: do NOT kill/recreate
      // it — re-setting the same hostname restarts the probe+announce
      // cycle instead, putting a fresh A record with the CURRENT IP on
      // the air (client caches heal, a renewed DHCP lease is covered).
      // No mdns_free/mdns_init roulette, no dead window.
      static bool prevConn = false;
      if (!prevConn && mdnsUp) {
        if (mdns_hostname_set(settings.hostname.c_str()) == ESP_OK)
          Serial.println(F("[net] mDNS: re-announced after reconnect"));
        else
          Serial.println(F("[net] mDNS: re-announce FAILED"));
      }
      prevConn = true;
    }

    if (!mdnsUp) {
      static uint32_t mdnsRetry = 0;  // don't hammer MDNS.begin() every loop
      if ((int32_t)(millis() - mdnsRetry) >= 0) {
        if (MDNS.begin(settings.hostname.c_str())) {
          MDNS.addService("http", "tcp", 80);
          mdnsUp = true;
          mdnsRejoin();  // lwip still "remembers" the group: no fresh IGMP
                         // report would leave — force one (snooping routers)
          Serial.printf("[net] mDNS: http://%s.local\n", settings.hostname.c_str());
        } else {
          mdnsRetry = millis() + 5000;
          // A silent failure here is EXACTLY "page dead by name, alive by
          // IP" — it must be visible in the log (another device claiming
          // the same hostname, or a wedged stack after too many restarts)
          Serial.println(F("[net] mDNS: begin FAILED, retrying in 5s"));
        }
      }
    }
    // NOTE: no periodic MDNS.end()/begin() "re-announce" cycles anymore —
    // neither the old 5-minute timer nor the per-drop teardown. Both rolled
    // the mdns_init()-after-mdns_free() dice hundreds of times a day (the
    // hidden-SSID reconnect loop made it worse), and one lost roll leaves
    // the responder deaf permanently and silently. The stack is created
    // once per connect-lifetime, survives STA drops, and is re-announced
    // softly on reconnect (hostname_set above). The IGMP snooping angle is
    // handled by the membership refresh above; the watchdog only observes
    // (see the telemetry note in this file) — automated mDNS "fixes" based
    // on an unreliable health signal did real damage and are banned.
    if (portal && settings.wifiConfigured) {
      // grace window: the setup page (still on our AP) polls /api/status and
      // grabs sta.ip — its follow-up link must be the IP, not <host>.local
      // (Android has no mDNS resolver and would lose the device)
      if (!portalGraceAt) {
        portalGraceAt = millis() + PORTAL_GRACE_MS;
        Serial.printf("[net] portal: STA up, closing AP in %us\n",
                      (unsigned)(PORTAL_GRACE_MS / 1000));
      } else if ((int32_t)(millis() - portalGraceAt) >= 0) {
        portalGraceAt = 0;
        stopPortal();
      }
    }
  } else {
    portalGraceAt = 0;  // STA dropped mid-grace: keep the portal, retry below
    // NOTE: mDNS is NOT torn down on a drop anymore. The old MDNS.end()
    // here + MDNS.begin() on the reconnect rolled the mdns_init()-after-
    // mdns_free() dice on EVERY reconnect: with a hidden SSID (unreliable
    // reconnects -> BEACON_TIMEOUT loops) that is dozens of dice rolls a
    // day, and one of them eventually leaves the responder deaf forever —
    // ".local dead, IP works". The stack now survives drops untouched;
    // the reconnect path below re-announces it softly instead.
    // A portal-side /api/networks scan silently flips the mode to AP_STA
    // (the core's scanNetworks() force-enables STA for every scan): after
    // the scan is done (or wedged), drop STA again — the portal beacon
    // must stay rock stable, and the retry branch below is the only one
    // allowed to re-enable it.
    // BOTH guards are required: the core's SCANNING bit is a LIE after its
    // max_ms_per_chan*20 timeout (2.4 s at our dwell) while the driver
    // keeps scanning for 10-20+ s with a client parked on our AP — killing
    // STA in that window aborted the live scan and the portal showed zero
    // networks. webScanRecent() covers the whole real scan lifetime.
    if (portal && !retryAt && WiFi.getMode() == WIFI_AP_STA &&
        WiFi.scanComplete() != WIFI_SCAN_RUNNING && !webScanRecent()) {
      WiFi.mode(WIFI_AP);
      txApplyCeiling();  // mode switch: keep the board ceiling
    }
    if (settings.wifiConfigured) {
      if (!portal) {
        bool timeout = (connecting && millis() - tConnect > 20000) ||
                       (!connecting && lastConn && millis() - lastConn > 20000);
        if (failCount >= 3 && (int32_t)(millis() - directedNextAt) >= 0) {
          // hidden AP / stubborn NO_AP_FOUND: probe for the exact SSID
          // and connect by BSSID before giving up to the portal
          tryDirectedConnect();
        } else if (failCount >= 10 || timeout) {
          // 10 rejected attempts or 20s of silence -> full portal (AP only,
          // STA off): the beacon is rock stable, phones always see it
          startPortal();
        }
      } else if (retryAt) {
        if (failCount >= 3 && (int32_t)(millis() - directedNextAt) >= 0) {
          // same directed-SSIDs fallback inside the 15s portal retry window
          tryDirectedConnect();
          retryAt = millis() + 15000;  // the scan itself eats ~2s of it
        } else if ((int32_t)(millis() - retryAt) >= 0) {
          // a short STA attempt (right after saving Wi-Fi / the periodic
          // retry) is in flight; give it 15s, then drop STA again
          retryAt = 0;
          WiFi.disconnect(false);
          WiFi.mode(WIFI_AP);
          txApplyCeiling();  // mode switch: keep the board ceiling
          Serial.println(F("[net] portal: STA retry failed, back to AP only"));
          tConnect = millis();
        }
      } else if (millis() - tConnect > 120000) {
        // once every 2 minutes: a short STA attempt in case the router just
        // rebooted — the device recovers on its own instead of sitting in
        // the portal forever (the AP blinks for these 15s, that's the price)
        Serial.println(F("[net] portal: periodic STA retry"));
        WiFi.mode(WIFI_AP_STA);
        WiFi.setHostname(settings.hostname.c_str());  // keep the DHCP name alive
        txApplyCeiling();  // mode switch: keep the board ceiling
        WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
        retryAt = millis() + 15000;
      }
    }
  }
}

bool netIsPortal() {
  return portal;
}

// last mDNS self-check verdict (watchdog task): false = the responder did
// not answer its own name over the air — ".local dead, IP alive"
bool netMdnsSelfOk() {
  return mdnsSelfOk;
}

// true = the portal AP failed to start (all softAP retries): phones have
// nothing to connect to, the web UI is unreachable. Reported via
// /api/status (ap.failed) and shown on the display.
bool netApFailed() {
  return portal && apFailed;
}

// /api/wifi from the portal: connect to the saved network WITHOUT a reboot.
// The AP stays up (netLoop drops it PORTAL_GRACE_MS after STA connects), so
// the phone keeps the page open, learns the new IP and follows the device —
// with a reboot it would strand on the dead 192.168.4.1 origin and have to
// guess <host>.local, which Android cannot resolve at all.
void netPortalTrySta() {
  if (!portal) return;
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.mode(WIFI_AP_STA);
  txApplyCeiling();  // mode switch: keep the board ceiling
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  connecting = true;
  tConnect = millis();
  retryAt = millis() + 15000;  // wrong password etc: back to AP-only
  Serial.printf("[net] portal: trying STA \"%s\"\n", settings.wifiSsid.c_str());
}

// Runtime TX power entry point: manual=true (serial "TXPWR") exempts the
// value from the ceiling guard until reboot — see the ceiling block on top.
void netSetTxPower(float dbm, bool manual) {
  // ONLY exactly 0 means "not set" = max. A REAL negative value (serial
  // "TXPWR -1") must reach txPowerLevel() and map to WIFI_POWER_MINUS_1dBm:
  // the antenna/SWR diagnostics depend on the lowest step actually working
  // — eating it here made "TXPWR -1" silently transmit at 19.5 dBm.
  if (dbm == 0) dbm = 19.5f;
  wifi_power_t lvl = txPowerLevel(dbm);
  if (manual) {
    // deliberate diagnostic override: exempt from the ceiling guard until
    // reboot, but say so loudly — above the ceiling the antenna chokes
    if (lvl > txPowerLevel(txCeilDbm))
      Serial.printf("[net] tx power MANUAL override ABOVE the %.1f dBm board ceiling (until reboot)\n",
                    txCeilDbm);
    txManual = true;
  }
  if (!WiFi.setTxPower(lvl))
    Serial.println(F("[net] set tx power FAILED"));
}

uint8_t netLastDisconnectReason() { return lastDisc; }

const char *netLastDisconnectName() {
  return lastDisc ? WiFi.disconnectReasonName((wifi_err_reason_t)lastDisc) : "";
}
