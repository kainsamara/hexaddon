#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "settings.h"

static DNSServer dns;
static bool portal = false;
static bool mdnsUp = false;
static bool connecting = false;
static uint32_t tConnect = 0;
static uint8_t failCount = 0;    // consecutive rejected STA attempts
static uint32_t retryAt = 0;     // deadline of a periodic STA retry from the portal
static String apSsid;

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
  dns.start(53, "*", WiFi.softAPIP());
  portal = true;
  retryAt = 0;
  failCount = 0;
  tConnect = millis();
  Serial.printf("[net] setup portal: \"%s\" ip=%s ap=%s ch=%d\n", apSsid.c_str(),
                WiFi.softAPIP().toString().c_str(), ap ? "up" : "FAIL",
                (int)WiFi.channel());
}

static void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // back to normal STA behavior
  portal = false;
  apSsid = "";
  Serial.println(F("[net] portal closed, STA connected"));
}

void netBegin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
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
}

void netLoop() {
  static int lastSta = -1;
  static uint32_t lastConn = 0;  // last moment STA was connected (0 = never)
  int st = (int)WiFi.status();
  if (st != lastSta) {
    lastSta = st;
    Serial.printf("[net] sta status=%d ip=%s\n", st, WiFi.localIP().toString().c_str());
  }
  if (portal) dns.processNextRequest();

  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn) {
    connecting = false;
    lastConn = millis();
    lastDisc = 0;  // stale reason must not surface after a success
    failCount = 0;
    if (!mdnsUp) {
      static uint32_t mdnsRetry = 0;  // don't hammer MDNS.begin() every loop
      if ((int32_t)(millis() - mdnsRetry) >= 0) {
        if (MDNS.begin(settings.hostname.c_str())) {
          MDNS.addService("http", "tcp", 80);
          mdnsUp = true;
          Serial.printf("[net] mDNS: http://%s.local\n", settings.hostname.c_str());
        } else {
          mdnsRetry = millis() + 5000;
        }
      }
    }
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
    if (settings.wifiConfigured) {
      if (!portal) {
        bool timeout = (connecting && millis() - tConnect > 20000) ||
                       (!connecting && lastConn && millis() - lastConn > 20000);
        // 10 rejected attempts or 20s of silence -> full portal (AP only,
        // STA off): the beacon is rock stable, phones always see it
        if (failCount >= 10 || timeout) startPortal();
      } else if (retryAt) {
        // a short STA attempt (right after saving Wi-Fi / the periodic retry)
        // is in flight; give it 15s, then drop STA again
        if ((int32_t)(millis() - retryAt) >= 0) {
          retryAt = 0;
          WiFi.disconnect(false);
          WiFi.mode(WIFI_AP);
          Serial.println(F("[net] portal: STA retry failed, back to AP only"));
          tConnect = millis();
        }
      } else if (millis() - tConnect > 120000) {
        // once every 2 minutes: a short STA attempt in case the router just
        // rebooted — the device recovers on its own instead of sitting in
        // the portal forever (the AP blinks for these 15s, that's the price)
        Serial.println(F("[net] portal: periodic STA retry"));
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
        retryAt = millis() + 15000;
      }
    }
  }
}

bool netIsPortal() {
  return portal;
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
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  connecting = true;
  tConnect = millis();
  retryAt = millis() + 15000;  // wrong password etc: back to AP-only
  Serial.printf("[net] portal: trying STA \"%s\"\n", settings.wifiSsid.c_str());
}

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

void netSetTxPower(float dbm) {
  if (dbm <= 0) dbm = 19.5f;
  WiFi.setTxPower(txPowerLevel(dbm));
}

uint8_t netLastDisconnectReason() { return lastDisc; }

const char *netLastDisconnectName() {
  return lastDisc ? WiFi.disconnectReasonName((wifi_err_reason_t)lastDisc) : "";
}
