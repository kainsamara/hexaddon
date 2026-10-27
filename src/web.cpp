#include "web.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "leds.h"
#include "display.h"
#include "net.h"
#include "ota.h"
#include "sensor.h"
#include "settings.h"
#include "index_html.h"  // generated from data/index.html by scripts/embed_index.py
#include "favicon_svg.h"  // generated from data/favicon.svg by scripts/embed_index.py

// ===========================================================================
// Tiny synchronous HTTP/1.1 server on a plain WiFiServer.
//
// Why not ESPAsyncWebServer: that stack closes the TCP connection after
// EVERY response. With arduino-esp32 lwip defaults (16 tcp pcbs total,
// TIME_WAIT = 2 x 60 s) a single page polling /api/status every 2.5 s
// parks 48 pcbs in TIME_WAIT and exhausts the pool within ~40 s - every
// NEW client is then dropped at SYN level (Chrome shows
// ERR_TOO_MANY_RETRIES, other browsers just look hung).
//
// This server speaks proper keep-alive instead: connections stay
// ESTABLISHED for minutes and TIME_WAIT is never produced. Everything -
// socket reads, parsing, handlers, JSON - runs inside webLoop() in the
// main Arduino loop, the same context as settings/display/leds, so there
// are no cross-task races by construction. Intended load: 1-2 clients.
// ===========================================================================

#define HTTP_MAX_CLIENTS 6
#define REQ_HEAD_MAX 2048    // request line + headers
#define BODY_MAX 8192        // JSON bodies; /api/update streams instead
#define KEEPALIVE_IDLE_MS 6000
#define BODY_STALL_MS 15000  // body announced but stopped arriving

static WiFiServer http(80);
static uint32_t restartAt = 0;
static uint8_t imgBuf[IMG_SIZE];

// loop-context side effects (I2C/NeoPixel) — plain byte, single context
enum ApplyBits : uint8_t {
  EFF_PINS = 1,   // settings.applyPins()
  EFF_LEDS = 2,   // ledsApply()
  EFF_DRV = 4,    // displayReinit()
  EFF_FLIP = 8,   // displayApplyFlip()
};
static uint8_t applyEffects = 0;

static void scheduleRestart(uint32_t delayMs = 500) {
  restartAt = millis() + delayMs;
}

// ---------------- connection slots ----------------

struct HttpSlot {
  WiFiClient cli;
  String head;  // request line + headers while being read
  String method, path, query;
  uint8_t body[BODY_MAX];
  size_t bodyLen = 0, contentLen = 0;
  bool headDone = false;
  bool keepAlive = true, stream = false;  // stream = /api/update -> Update.h
  uint32_t lastActivity = 0;
};
static HttpSlot slot[HTTP_MAX_CLIENTS];

static void resetSlot(HttpSlot &s, bool close) {
  if (close) {
    s.cli.stop();
    s.cli = WiFiClient();
  }
  s.head = "";
  s.method = s.path = s.query = "";
  s.bodyLen = 0;
  s.contentLen = 0;
  s.headDone = false;
  s.stream = false;
}

// ---------------- response helpers ----------------

static const char *statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

// blocking write of the whole buffer; false when the client died
static bool writeAll(WiFiClient &cli, const uint8_t *d, size_t n) {
  size_t off = 0;
  uint32_t stall = millis();
  while (off < n) {
    if (!cli.connected()) return false;
    size_t w = cli.write(d + off, n - off);
    if (w) {
      off += w;
      stall = millis();
    } else if (millis() - stall > 3000) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

static bool respond(HttpSlot &s, int code, const char *type, const uint8_t *body,
                    size_t len, const char *cache = "no-store",
                    const char *extraHeader = nullptr) {
  String h = "HTTP/1.1 ";
  h += code;
  h += ' ';
  h += statusText(code);
  h += "\r\nContent-Type: ";
  h += type;
  h += "\r\nContent-Length: ";
  h += (unsigned)len;
  h += "\r\nConnection: ";
  h += s.keepAlive ? "keep-alive" : "close";
  h += "\r\nCache-Control: ";
  h += cache;
  h += "\r\n";
  if (extraHeader) h += extraHeader;
  h += "\r\n";
  if (!writeAll(s.cli, (const uint8_t *)h.c_str(), h.length())) return false;
  if (body && len && !writeAll(s.cli, body, len)) return false;
  return true;
}

static bool respondStr(HttpSlot &s, int code, const char *type, const String &b,
                       const char *cache = "no-store") {
  return respond(s, code, type, (const uint8_t *)b.c_str(), b.length(), cache);
}

static bool sendJson(HttpSlot &s, int code, const String &b) {
  return respondStr(s, code, "application/json", b);
}

static bool sendErr(HttpSlot &s, int code, const char *msg) {
  return sendJson(s, code, String("{\"error\":\"") + msg + "\"}");
}

static bool sendRedirect(HttpSlot &s, const String &to) {
  String loc = "Location: " + to + "\r\n";
  return respond(s, 302, "text/plain", nullptr, 0, "no-store", loc.c_str());
}

static String queryParam(const String &q, const char *name) {
  int pos = 0;
  while (pos >= 0 && pos < (int)q.length()) {
    int amp = q.indexOf('&', pos);
    String pair = (amp < 0) ? q.substring(pos) : q.substring(pos, amp);
    int eq = pair.indexOf('=');
    if (eq > 0 && pair.substring(0, eq) == name) {
      String v = pair.substring(eq + 1), out;
      for (int i = 0; i < (int)v.length(); i++) {
        char ch = v[i];
        if (ch == '+') {
          out += ' ';
        } else if (ch == '%' && i + 2 < (int)v.length()) {
          out += (char)strtol(v.substring(i + 1, i + 3).c_str(), nullptr, 16);
          i += 2;
        } else {
          out += ch;
        }
      }
      return out;
    }
    pos = (amp < 0) ? -1 : amp + 1;
  }
  return "";
}

// ---------------- hex helpers (no raw binary over JSON) ----------------

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static String toHex(const uint8_t *d, size_t n) {
  static const char *H = "0123456789ABCDEF";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += H[d[i] >> 4];
    s += H[d[i] & 0x0F];
  }
  return s;
}

static bool fromHex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    int a = hexVal(s[i * 2]);
    int b = hexVal(s[i * 2 + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)((a << 4) | b);
  }
  return true;
}

static String addrStr(uint8_t addr) {
  char b[6];
  snprintf(b, sizeof(b), "0x%02X", addr);
  return String(b);
}

// accepts "0x3C"/"0x3D" (and plain 60/61)
static bool parseAddr(const String &s, uint8_t &out) {
  if (!s.length()) return false;
  long v = strtol(s.c_str(), nullptr, 0);
  if (v == DISP_ADDR0 || v == DISP_ADDR1) {
    out = (uint8_t)v;
    return true;
  }
  return false;
}

static String bodyStr(HttpSlot &s) {
  return String((const char *)s.body, s.bodyLen);
}

// ---------------- json ----------------

static void fillStatus(JsonDocument &d) {
  d["ok"] = true;
  d["version"] = FW_VERSION;
  d["build"] = buildStamp();  // tells local builds apart (same FW_VERSION)
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["portal"] = netIsPortal();
  d["wifiConfigured"] = settings.wifiConfigured;

  JsonObject sta = d["sta"].to<JsonObject>();
  sta["connected"] = (WiFi.status() == WL_CONNECTED);
  // full portal: the STA interface is off and forgets its SSID — take it
  // from the saved settings so the page can say which network failed
  String ssid = WiFi.SSID();
  if (!ssid.length()) ssid = settings.wifiSsid;
  sta["ssid"] = ssid;
  sta["ip"] = WiFi.localIP().toString();
  sta["rssi"] = WiFi.RSSI();
  sta["mac"] = WiFi.macAddress();
  sta["hostname"] = settings.hostname;
  // why the last connection attempt failed (shown on the portal page)
  sta["disconnect"] = netLastDisconnectReason();
  sta["disconnectName"] = netLastDisconnectName();

  if (netIsPortal()) {
    JsonObject ap = d["ap"].to<JsonObject>();
    ap["active"] = true;
    ap["ssid"] = WiFi.softAPSSID();
    ap["ip"] = WiFi.softAPIP().toString();
    ap["clients"] = WiFi.softAPgetStationNum();
  } else {
    d["ap"]["active"] = false;
  }

  d["panels"] = settings.panels;
  d["leds"] = settings.ledCount();
  d["anim"] = settings.anim;
  d["animOn"] = settings.animOn;
  d["brightness"] = settings.brightness;
  {
    JsonArray ma = d["displayModes"].to<JsonArray>();
    ma.add(dispModeStr(settings.displayMode[0]));
    ma.add(dispModeStr(settings.displayMode[1]));
  }
  {
    JsonArray ap = d["animPresets"].to<JsonArray>();
    ap.add(animPresetStr(settings.animPreset[0]));
    ap.add(animPresetStr(settings.animPreset[1]));
  }
  d["addonPower"] = settings.addonPower;
  d["ledMa"] = ledsCurrentMa();  // live strip draw, for the Панели tab

  uint8_t plist[DISP_NUM];
  int pn = displayPresentList(plist, DISP_NUM);
  JsonArray da = d["displays"].to<JsonArray>();
  for (int i = 0; i < pn; i++) {
    JsonObject o = da.add<JsonObject>();
    o["addr"] = addrStr(plist[i]);
    o["hasImage"] = displayHasImage(plist[i]);
  }

  JsonObject g0 = d["gpio0"].to<JsonObject>();
  g0["mode"] = gpioModeStr(settings.gpio0.mode);
  g0["value"] = settings.gpio0.value;
  g0["state"] = digitalRead(PIN_IO0);
  JsonObject g1 = d["gpio1"].to<JsonObject>();
  g1["mode"] = gpioModeStr(settings.gpio1.mode);
  g1["value"] = settings.gpio1.value;
  g1["state"] = digitalRead(PIN_IO1);

  JsonObject sen = d["sensor"].to<JsonObject>();
  sen["present"] = sensorPresent();
  if (sensorPresent()) {
    sen["addr"] = addrStr(sensorAddr());
    sen["chip"] = sensorChip();
    if (!isnan(sensorTemp())) {
      sen["temp"] = (double)sensorTemp();
      if (!isnan(sensorHum())) sen["hum"] = (double)sensorHum();
      sen["pres"] = (double)sensorPres();
    }
  }

  JsonArray an = d["anims"].to<JsonArray>();
  for (int i = 0; i < NUM_ANIMS; i++) an.add(ledsAnimName(i));
}

static void fillConfig(JsonDocument &d) {
  d["panels"] = settings.panels;
  d["reverse"] = settings.reverse;
  d["brightness"] = settings.brightness;
  d["speed"] = settings.speed;
  d["anim"] = settings.anim;
  d["animOn"] = settings.animOn;
  d["scope"] = (settings.scope == Scope::Panel) ? "panel" : "chain";
  d["color"] = settings.colorHex();
  d["colorRandom"] = settings.colorRandom;
  d["order"] = settings.order;
  d["powerMa"] = settings.powerMa;
  d["addonPower"] = settings.addonPower;
  d["gpio0"]["mode"] = gpioModeStr(settings.gpio0.mode);
  d["gpio0"]["value"] = settings.gpio0.value;
  d["gpio1"]["mode"] = gpioModeStr(settings.gpio1.mode);
  d["gpio1"]["value"] = settings.gpio1.value;
  JsonArray ma = d["displayModes"].to<JsonArray>();
  ma.add(dispModeStr(settings.displayMode[0]));
  ma.add(dispModeStr(settings.displayMode[1]));
  JsonArray dr = d["drivers"].to<JsonArray>();
  dr.add(settings.driver[0]);
  dr.add(settings.driver[1]);
  JsonArray fa = d["flips"].to<JsonArray>();
  JsonArray apa = d["animPresets"].to<JsonArray>();
  apa.add(animPresetStr(settings.animPreset[0]));
  apa.add(animPresetStr(settings.animPreset[1]));
  fa.add(settings.flip[0]);
  fa.add(settings.flip[1]);
  JsonArray ba = d["btns"].to<JsonArray>();
  for (int i = 0; i < BTN_SLOTS; i++) ba.add(btnActionStr(settings.btnAct[i]));
  d["tempOffset"] = settings.tempOffset;
  {
    JsonArray sa = d["sched"].to<JsonArray>();
    for (int i = 0; i < MAX_SCHED_RULES; i++) {
      const SchedRule &r = settings.sched[i];
      if (!r.used) continue;
      JsonObject o = sa.add<JsonObject>();
      o["on"] = r.on;
      o["days"] = r.days;
      o["h"] = r.hour;
      o["m"] = r.minute;
      o["act"] = r.enable;
      o["dur"] = r.durationMin;
      o["tgt"] = r.target ? "power" : "light";
    }
  }
  d["hostname"] = settings.hostname;
  d["tz"] = settings.tz;
  d["theme"] = settings.theme;
}

static String docStr(JsonDocument &d) {
  String out;
  serializeJson(d, out);
  return out;
}

static bool applyConfig(JsonDocument &d) {
  Settings &s = settings;
  bool reboot = false;

  if (d["panels"].is<int>()) s.panels = constrain((int)d["panels"], 1, MAX_PANELS);
  if (d["reverse"].is<bool>()) s.reverse = d["reverse"].as<bool>();
  if (d["brightness"].is<int>()) s.brightness = constrain((int)d["brightness"], 0, 255);
  if (d["speed"].is<int>()) s.speed = constrain((int)d["speed"], 0, 100);
  if (d["anim"].is<int>()) s.anim = constrain((int)d["anim"], 0, NUM_ANIMS - 1);
  if (d["animOn"].is<bool>()) s.animOn = d["animOn"].as<bool>();
  if (d["scope"].is<const char *>())
    s.scope = (String(d["scope"].as<const char *>()) == "chain") ? Scope::Chain
                                                                 : Scope::Panel;
  if (d["color"].is<const char *>()) s.setColorHex(d["color"].as<String>());
  if (d["colorRandom"].is<bool>()) s.colorRandom = d["colorRandom"].as<bool>();
  if (d["order"].is<const char *>()) s.order = d["order"].as<String>();
  if (d["powerMa"].is<int>()) s.powerMa = constrain((int)d["powerMa"], 0, 20000);
  if (d["addonPower"].is<bool>()) {
    s.addonPower = d["addonPower"].as<bool>();
    applyEffects |= EFF_PINS;  // power rail pin
  }

  if (d["gpio0"].is<JsonObjectConst>()) {
    JsonObjectConst g = d["gpio0"];
    if (g["mode"].is<const char *>()) s.gpio0.mode = gpioModeFromStr(g["mode"].as<String>());
    if (g["value"].is<int>()) s.gpio0.value = g["value"] ? 1 : 0;
    applyEffects |= EFF_PINS;
  }
  if (d["gpio1"].is<JsonObjectConst>()) {
    JsonObjectConst g = d["gpio1"];
    if (g["mode"].is<const char *>()) s.gpio1.mode = gpioModeFromStr(g["mode"].as<String>());
    if (g["value"].is<int>()) s.gpio1.value = g["value"] ? 1 : 0;
    applyEffects |= EFF_PINS;
  }

  if (d["displayModes"].is<JsonArray>()) {
    JsonArray ma = d["displayModes"].as<JsonArray>();
    if (ma.size() > 0) s.displayMode[0] = dispModeFromStr(String(ma[0] | "info"));
    if (ma.size() > 1) s.displayMode[1] = dispModeFromStr(String(ma[1] | "info"));
  } else if (d["displayMode"].is<const char *>()) {
    DispMode m = dispModeFromStr(d["displayMode"].as<String>());
    s.displayMode[0] = s.displayMode[1] = m;
  }

  bool drvChanged = false;
  if (d["drivers"].is<JsonArray>()) {
    JsonArray dr = d["drivers"].as<JsonArray>();
    for (int i = 0; i < 2 && i < (int)dr.size(); i++) {
      String drv = String(dr[i] | "ssd1306");
      if (drv == "ssd1306" || drv == "sh1106") {
        drvChanged |= (drv != s.driver[i]);
        s.driver[i] = drv;
      }
    }
  } else if (d["driver"].is<const char *>()) {
    String drv = d["driver"].as<String>();
    if (drv == "ssd1306" || drv == "sh1106") {
      drvChanged = (drv != s.driver[0]) || (drv != s.driver[1]);
      s.driver[0] = s.driver[1] = drv;
    }
  }
  if (drvChanged) applyEffects |= EFF_DRV;

  if (d["btns"].is<JsonArray>()) {
    JsonArray ba = d["btns"].as<JsonArray>();
    for (int i = 0; i < BTN_SLOTS && i < (int)ba.size(); i++)
      s.btnAct[i] = btnActionFromStr(String(ba[i] | ""), s.btnAct[i]);
  }

  if (d["animPresets"].is<JsonArray>()) {
    JsonArray apa = d["animPresets"].as<JsonArray>();
    if (apa.size() > 0) s.animPreset[0] = animPresetFromStr(String(apa[0] | "offzone2024"));
    if (apa.size() > 1) s.animPreset[1] = animPresetFromStr(String(apa[1] | "offzone2024"));
  }

  if (d["flips"].is<JsonArray>()) {
    JsonArray fa = d["flips"].as<JsonArray>();
    bool changed = false;
    if (fa.size() > 0) {
      bool v = fa[0] | false;
      changed |= (v != s.flip[0]);
      s.flip[0] = v;
    }
    if (fa.size() > 1) {
      bool v = fa[1] | false;
      changed |= (v != s.flip[1]);
      s.flip[1] = v;
    }
    if (changed) applyEffects |= EFF_FLIP;
  }
  if (d["tempOffset"].is<float>() || d["tempOffset"].is<int>())
    s.tempOffset = constrain((float)d["tempOffset"].as<float>(), -15.0f, 15.0f);
  if (d["sched"].is<JsonArray>()) {
    // the UI sends the full live list — free slots first, then fill in order
    for (int i = 0; i < MAX_SCHED_RULES; i++) s.sched[i] = SchedRule();
    JsonArray sa = d["sched"].as<JsonArray>();
    int k = 0;
    for (int i = 0; i < (int)sa.size() && k < MAX_SCHED_RULES; i++) {
      JsonObjectConst o = sa[i];
      if (o.isNull()) continue;
      SchedRule &r = s.sched[k++];
      r.used = true;
      if (o["on"].is<bool>()) r.on = o["on"].as<bool>();
      if (o["days"].is<int>()) r.days = (uint8_t)constrain((int)o["days"], 0, 127);
      if (o["h"].is<int>()) r.hour = (uint8_t)constrain((int)o["h"], 0, 23);
      if (o["m"].is<int>()) r.minute = (uint8_t)constrain((int)o["m"], 0, 59);
      if (o["act"].is<bool>()) r.enable = o["act"].as<bool>();
      if (o["dur"].is<int>()) r.durationMin = (uint16_t)constrain((int)o["dur"], 0, 1440);
      if (o["tgt"].is<const char *>())
        r.target = String(o["tgt"].as<const char *>()) == "power" ? 1 : 0;
    }
  }
  if (d["tz"].is<const char *>()) s.tz = d["tz"].as<String>();  // sanitized below
  if (d["theme"].is<const char *>()) s.theme = d["theme"].as<String>();

  if (d["hostname"].is<const char *>()) {
    String old = s.hostname;
    s.hostname = d["hostname"].as<String>();
    s.sanitize();  // cosmetic edits (case, bad chars) must not force a reboot
    if (s.hostname != old) reboot = true;
  }

  s.sanitize();
  s.save();
  applyEffects |= EFF_PINS | EFF_LEDS;  // cheap, idempotent
  return reboot;
}

// ---------------- handlers ----------------

static void handleStatus(HttpSlot &s) {
  JsonDocument d;
  fillStatus(d);
  sendJson(s, 200, docStr(d));
}

static void handleConfigGet(HttpSlot &s) {
  JsonDocument d;
  fillConfig(d);
  sendJson(s, 200, docStr(d));
}

static void handleConfigPost(HttpSlot &s) {
  JsonDocument d;
  if (deserializeJson(d, bodyStr(s))) {
    sendErr(s, 400, "bad json");
    return;
  }
  bool reboot = applyConfig(d);
  sendJson(s, 200, reboot ? "{\"ok\":true,\"reboot\":true}" : "{\"ok\":true}");
  if (reboot) scheduleRestart();
}

// ---------------- wifi ----------------

// Async scan: the first request starts it, polling requests collect the
// result. (Everything runs in the same loop, the async scan just keeps
// the radio sweeping in the background instead of blocking us.)
static bool scanStarted = false;

static void handleNetworks(HttpSlot &s) {
  if (!scanStarted) {
    WiFi.scanNetworks(true);
    scanStarted = true;
    sendJson(s, 202, "{\"scanning\":true}");
    return;
  }
  int st = WiFi.scanComplete();
  if (st == WIFI_SCAN_RUNNING) {
    sendJson(s, 202, "{\"scanning\":true}");
    return;
  }
  if (st < 0) {  // failed / not reported: restart on the next request
    scanStarted = false;
    sendJson(s, 202, "{\"scanning\":true}");
    return;
  }
  int n = st;
  struct Net {
    String ssid;
    int32_t rssi;
    bool sec;
  };
  Net list[24];
  int cnt = 0;
  for (int i = 0; i < n && cnt < 24; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool sec = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    int32_t rssi = WiFi.RSSI(i);
    bool dup = false;
    for (int j = 0; j < cnt; j++) {
      if (list[j].ssid == s) {
        if (rssi > list[j].rssi) {
          list[j].rssi = rssi;
          list[j].sec = sec;
        }
        dup = true;
        break;
      }
    }
    if (!dup) {
      list[cnt].ssid = s;
      list[cnt].rssi = rssi;
      list[cnt].sec = sec;
      cnt++;
    }
  }
  for (int i = 0; i < cnt; i++) {
    for (int j = i + 1; j < cnt; j++) {
      if (list[j].rssi > list[i].rssi) {
        Net t = list[i];
        list[i] = list[j];
        list[j] = t;
      }
    }
  }
  WiFi.scanDelete();
  scanStarted = false;

  JsonDocument d;
  JsonArray arr = d["networks"].to<JsonArray>();
  for (int i = 0; i < cnt; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = list[i].ssid;
    o["rssi"] = list[i].rssi;
    o["secure"] = list[i].sec;
  }
  sendJson(s, 200, docStr(d));
}

static void handleWifiPost(HttpSlot &s) {
  JsonDocument d;
  if (deserializeJson(d, bodyStr(s))) {
    sendErr(s, 400, "bad json");
    return;
  }
  String ssid = d["ssid"] | "";
  String pass = d["pass"] | "";
  if (!ssid.length()) {
    sendErr(s, 400, "empty ssid");
    return;
  }
  if (ssid.length() > 32 || pass.length() > 64) {
    sendErr(s, 400, "ssid/pass too long");
    return;
  }
  settings.setWifi(ssid, pass);
  if (netIsPortal()) {
    // no reboot: keep the AP (and the phone's page) alive through the
    // transition — netLoop closes the portal once STA connects, the page
    // follows the device by its new IP (works on Android too, .local doesn't)
    netPortalTrySta();
    sendJson(s, 200, "{\"ok\":true,\"msg\":\"Сохранено, подключаюсь...\"}");
  } else {
    // changing Wi-Fi from the normal UI: plain reboot into the new network
    sendJson(s, 200, "{\"ok\":true,\"msg\":\"Сохранено, перезагрузка...\"}");
    scheduleRestart(700);
  }
}

static void handleWifiForget(HttpSlot &s) {
  settings.clearWifi();
  sendJson(s, 200, "{\"ok\":true,\"msg\":\"Сеть забыта\"}");
  scheduleRestart(700);
}

// ---------------- display image ----------------

static void handleImageGet(HttpSlot &s) {
  uint8_t addr;
  if (!parseAddr(queryParam(s.query, "addr"), addr)) {
    sendErr(s, 400, "addr=0x3C|0x3D");
    return;
  }
  static uint8_t buf[IMG_SIZE];  // loop context, no reentry -> .bss, not stack
  ::memset(buf, 0, IMG_SIZE);
  bool exists = displayHasImage(addr);
  if (exists) displayCopyImage(addr, buf);  JsonDocument d;
  d["addr"] = addrStr(addr);
  d["exists"] = exists;
  d["size"] = IMG_SIZE;
  d["data"] = toHex(buf, IMG_SIZE);
  sendJson(s, 200, docStr(d));
}

static void handleImagePost(HttpSlot &s) {
  JsonDocument d;
  if (deserializeJson(d, bodyStr(s))) {
    sendErr(s, 400, "bad json");
    return;
  }
  uint8_t addr;
  if (!parseAddr(d["addr"] | "", addr)) {
    sendErr(s, 400, "addr=0x3C|0x3D");
    return;
  }
  String data = d["data"] | "";
  if (!fromHex(data, imgBuf, IMG_SIZE)) {
    sendErr(s, 400, "need 1024 bytes hex");
    return;
  }
  if (!displaySaveImage(addr, imgBuf, IMG_SIZE)) {
    sendErr(s, 500, "save failed");
    return;
  }
  displayToast("Image " + addrStr(addr));
  sendJson(s, 200, "{\"ok\":true}");
}

static void handleImageDelete(HttpSlot &s) {
  uint8_t addr;
  if (!parseAddr(queryParam(s.query, "addr"), addr)) {
    sendErr(s, 400, "addr=0x3C|0x3D");
    return;
  }
  displayClearImage(addr);
  displayToast("Deleted " + addrStr(addr));
  sendJson(s, 200, "{\"ok\":true}");
}

// ---------------- system ----------------

static void handleReboot(HttpSlot &s) {
  sendJson(s, 200, "{\"ok\":true}");
  scheduleRestart(600);
}

static void handleSensorHistory(HttpSlot &s) {
  static int16_t bt[HIST_N], bh[HIST_N];  // .bss, not stack
  static uint32_t bts[HIST_N];
  int n = sensorHistory(bt, bh, bts, HIST_N);
  JsonDocument d;
  d["interval"] = HIST_STEP_MIN;  // minutes between points
  d["now"] = (uint32_t)time(nullptr);
  JsonArray ta = d["t"].to<JsonArray>();
  JsonArray ha = d["h"].to<JsonArray>();
  JsonArray ts = d["ts"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    ta.add(bt[i]);
    ha.add(bh[i]);
    ts.add(bts[i]);
  }
  sendJson(s, 200, docStr(d));
}

static void handleFactory(HttpSlot &s) {
  settings.factoryReset();
  // RAM still holds the old config — a settings.save() in the 800ms
  // restart window (button action, /api/config) would write it back
  settings = Settings();
  LittleFS.remove("/img_3c.xbm");  // legacy images (pre-NVS firmware)
  LittleFS.remove("/img_3d.xbm");
  applyEffects |= EFF_PINS;
  sendJson(s, 200, "{\"ok\":true}");
  scheduleRestart(800);
}

// ---------------- OTA from the GitHub release feed ----------------

static void handleUpdateCheck(HttpSlot &s) {
  const OtaState o = otaState();  // mutex-protected snapshot
  // a failed check (no internet yet / rate limit) is retried after a minute,
  // a successful one is cached for the full TTL — «Ошибка проверки» must
  // not sit there for 10 minutes after the router comes back
  uint32_t ttl = o.netError ? 60000UL : CHECK_TTL_MS;
  if (o.checking || !o.checked || millis() - o.checkedAt > ttl) {
    if (!o.checking) otaCheckAsync();
    sendJson(s, 202, "{\"checking\":true}");
    return;
  }
  if (o.netError) {
    sendJson(s, 200, "{\"current\":\"" + String(FW_VERSION) + "\",\"error\":\"check failed\"}");
    return;
  }
  JsonDocument d;
  d["current"] = FW_VERSION;
  d["latest"] = o.latest;
  d["available"] = otaNewerThan(o.latest);
  sendJson(s, 200, docStr(d));
}

static void handleOtaStart(HttpSlot &s) {
  String err;
  if (!otaStart(err)) {
    sendErr(s, 409, err.c_str());
    return;
  }
  sendJson(s, 202, "{\"ok\":true}");
}

static void handleOtaStatus(HttpSlot &s) {
  const OtaState o = otaState();  // mutex-protected snapshot
  JsonDocument d;
  d["state"] = o.stage;
  d["progress"] = o.progress;
  if (o.error.length()) d["error"] = o.error;
  sendJson(s, 200, docStr(d));
}

// ---------------- static ----------------

static void serveIndex(HttpSlot &s) {
  if (INDEX_HTML_GZ_LEN) {
    // embedded (gzip) page: OTA updates it together with the firmware
    respond(s, 200, "text/html; charset=utf-8", (const uint8_t *)INDEX_HTML_GZ,
            INDEX_HTML_GZ_LEN, "no-cache", "Content-Encoding: gzip\r\n");
    return;
  }
  // fallback for a hand-built firmware without the embedded page
  if (LittleFS.exists("/index.html")) {
    File f = LittleFS.open("/index.html", "r");
    if (f) {
      size_t len = f.size();
      String h = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: " + String((unsigned)len) +
                 "\r\nConnection: " + String(s.keepAlive ? "keep-alive" : "close") +
                 "\r\nCache-Control: no-cache\r\n\r\n";
      if (writeAll(s.cli, (const uint8_t *)h.c_str(), h.length())) {
        uint8_t buf[1024];
        while (len) {
          size_t n = f.read(buf, sizeof(buf));
          if (!n || !writeAll(s.cli, buf, n)) break;
          len -= n;
        }
      }
      f.close();
      return;
    }
  }
  respondStr(s, 500, "text/plain; charset=utf-8", "index.html is not built in");
}

static void serveFavicon(HttpSlot &s) {
  if (!FAVICON_SVG_LEN) {  // data/favicon.svg was missing at build time
    sendErr(s, 404, "no favicon");
    return;
  }
  respond(s, 200, "image/svg+xml", (const uint8_t *)FAVICON_SVG, FAVICON_SVG_LEN,
          "max-age=86400");
}

static void handleNotFound(HttpSlot &s) {
  if (netIsPortal()) {
    // captive portal: redirect probes and everything else to the setup page
    sendRedirect(s, String("http://") + WiFi.softAPIP().toString() + "/");
    return;
  }
  if (s.method == "GET" && !s.path.startsWith("/api/")) {
    serveIndex(s);
    return;
  }
  sendErr(s, 404, "not found");
}

// ---------------- request parsing / dispatch ----------------

static bool parseHead(HttpSlot &s) {
  int blank = s.head.indexOf("\r\n\r\n");
  int lineEnd = s.head.indexOf("\r\n");
  if (blank < 0 || lineEnd < 0) return false;
  String reqLine = s.head.substring(0, lineEnd);
  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.lastIndexOf(' ');
  if (sp1 <= 0 || sp2 <= sp1 + 1) return false;
  s.method = reqLine.substring(0, sp1);
  String target = reqLine.substring(sp1 + 1, sp2);
  String version = reqLine.substring(sp2 + 1);
  int q = target.indexOf('?');
  if (q >= 0) {
    s.query = target.substring(q + 1);
    target = target.substring(0, q);
  } else {
    s.query = "";
  }
  s.path = target;
  s.keepAlive = !version.equalsIgnoreCase("HTTP/1.0");
  s.contentLen = 0;

  int pos = lineEnd + 2;
  while (pos < blank) {
    int e = s.head.indexOf("\r\n", pos);
    if (e < 0 || e > blank) e = blank;
    String ln = s.head.substring(pos, e);
    int c = ln.indexOf(':');
    if (c > 0) {
      String name = ln.substring(0, c);
      String val = ln.substring(c + 1);
      name.trim();
      name.toLowerCase();
      val.trim();
      if (name == "content-length") {
        s.contentLen = (size_t)strtoull(val.c_str(), nullptr, 10);
      } else if (name == "connection") {
        String v = val;
        v.toLowerCase();
        if (v.indexOf("close") >= 0) s.keepAlive = false;
        else if (v.indexOf("keep-alive") >= 0) s.keepAlive = true;
      }
    }
    pos = e + 2;
  }
  return true;
}

static void dispatch(HttpSlot &s) {
  bool get = (s.method == "GET"), post = (s.method == "POST"),
       del = (s.method == "DELETE");
  const String &p = s.path;

  if (get && p == "/") return (void)serveIndex(s);
  if (get && (p == "/favicon.svg" || p == "/favicon.ico")) return (void)serveFavicon(s);
  if (get && p == "/api/status") return (void)handleStatus(s);
  if (get && p == "/api/sensor/history") return (void)handleSensorHistory(s);
  if (get && p == "/api/config") return (void)handleConfigGet(s);
  if (post && p == "/api/config") return (void)handleConfigPost(s);
  if (get && p == "/api/networks") return (void)handleNetworks(s);
  if (post && p == "/api/wifi") return (void)handleWifiPost(s);
  if (post && p == "/api/wifi/forget") return (void)handleWifiForget(s);
  if (get && p == "/api/display/image") return (void)handleImageGet(s);
  if (post && p == "/api/display/image") return (void)handleImagePost(s);
  if (del && p == "/api/display/image") return (void)handleImageDelete(s);
  if (post && p == "/api/reboot") return (void)handleReboot(s);
  if (post && p == "/api/factory") return (void)handleFactory(s);
  if (get && p == "/api/update/check") return (void)handleUpdateCheck(s);
  if (get && p == "/api/update/ota") return (void)handleOtaStatus(s);
  if (post && p == "/api/update/ota") return (void)handleOtaStart(s);
  handleNotFound(s);
}

// POST /api/upload — the firmware .bin as a RAW body (no multipart): the
// bytes are streamed straight into the inactive OTA slot as they arrive.
static bool startUpdateStream(HttpSlot &s) {
  if (otaDownloadActive() || Update.isRunning()) {
    sendErr(s, 409, "another update is in progress");
    return false;
  }
  if (!s.contentLen) {
    sendErr(s, 400, "empty body");
    return false;
  }
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    sendErr(s, 500, Update.errorString());
    return false;
  }
  s.stream = true;
  s.bodyLen = 0;
  Serial.printf("[ota] upload: streaming %u bytes\n", (unsigned)s.contentLen);
  return true;
}

static void finishUpdateStream(HttpSlot &s) {
  bool ok = Update.end(true);
  Serial.printf("[ota] upload: %u bytes, %s\n", (unsigned)s.bodyLen, ok ? "ok" : "FAIL");
  if (ok) {
    sendJson(s, 200, "{\"ok\":true}");
    scheduleRestart(600);
  } else {
    Update.printError(Serial);
    sendErr(s, 500, Update.errorString());
  }
}

static void serviceSlot(HttpSlot &s) {
  if (!s.cli) return;
  if (!s.cli.connected() && !s.cli.available()) {
    resetSlot(s, true);
    return;
  }

  uint32_t now = millis();
  int avail = s.cli.available();
  if (!avail) {
    if (s.contentLen) {  // mid-request: waiting for the body to arrive
      if (now - s.lastActivity > BODY_STALL_MS) {
        if (s.stream && Update.isRunning()) Update.end(false);
        resetSlot(s, true);
      }
    } else if (now - s.lastActivity > KEEPALIVE_IDLE_MS) {  // idle keep-alive
      resetSlot(s, true);
    }
    return;
  }
  s.lastActivity = now;

  if (!s.headDone) {
    while (s.cli.available()) {
      char c = (char)s.cli.read();
      s.head += c;
      if (s.head.length() >= 4 && s.head.endsWith("\r\n\r\n")) {
        s.headDone = true;
        break;
      }
      if (s.head.length() > REQ_HEAD_MAX) {
        s.keepAlive = false;
        sendErr(s, 431, "headers too large");
        resetSlot(s, true);
        return;
      }
    }
    if (!s.headDone) return;
    if (!parseHead(s)) {
      s.keepAlive = false;
      sendErr(s, 400, "bad request");
      resetSlot(s, true);
      return;
    }
    if (s.method == "POST" && s.path == "/api/update") {
      if (!startUpdateStream(s)) resetSlot(s, true);
      return;  // body bytes are streamed on the next passes
    }
    if (s.contentLen > BODY_MAX) {
      s.keepAlive = false;
      sendErr(s, 413, "too large");
      resetSlot(s, true);
      return;
    }
    if (!s.contentLen) {
      dispatch(s);
      resetSlot(s, !s.keepAlive);
      return;
    }
    return;  // body arrives on the next passes
  }

  if (s.stream) {  // /api/update: bytes go straight into the OTA slot
    uint8_t buf[1024];
    while (s.cli.available()) {
      int n = s.cli.read(buf, sizeof(buf));
      if (n <= 0) break;
      if (Update.write(buf, n) != (size_t)n) {
        Update.printError(Serial);
        Update.end(false);
        sendErr(s, 500, Update.errorString());
        resetSlot(s, true);
        return;
      }
      s.bodyLen += n;
      if (s.contentLen && s.bodyLen >= s.contentLen) break;
    }
    if (s.contentLen && s.bodyLen >= s.contentLen) {
      finishUpdateStream(s);
      resetSlot(s, true);  // device reboots anyway
    }
    return;
  }

  // regular body: accumulate until contentLen, then dispatch
  while (s.bodyLen < s.contentLen && s.cli.available()) {
    size_t want = s.contentLen - s.bodyLen;
    int n = s.cli.read(s.body + s.bodyLen, want);
    if (n <= 0) break;
    s.bodyLen += n;
  }
  if (s.bodyLen >= s.contentLen) {
    dispatch(s);
    resetSlot(s, !s.keepAlive);
  }
}

void webBegin() {
  http.begin();
  http.setNoDelay(true);
}

void webLoop() {
  // accept new clients into free slots. NOTE: never assign slot[i] =
  // HttpSlot() here — that would materialize an ~8 KB temporary on the
  // loopTask stack (8 KB by default) and blow it on the first client.
  while (http.hasClient()) {
    bool placed = false;
    for (int i = 0; i < HTTP_MAX_CLIENTS && !placed; i++) {
      if (!slot[i].cli) {
        resetSlot(slot[i], false);
        slot[i].cli = http.accept();
        slot[i].lastActivity = millis();
        placed = true;
      }
    }
    if (!placed) {  // full: accept and drop politely
      WiFiClient extra = http.accept();
      extra.stop();
    }
  }

  for (int i = 0; i < HTTP_MAX_CLIENTS; i++) serviceSlot(slot[i]);

  // side effects deferred out of handlers (kept for clarity only — the
  // handlers already run in the loop context here)
  if (applyEffects) {
    uint8_t eff = applyEffects;
    applyEffects = 0;
    if (eff & EFF_PINS) settings.applyPins();
    if (eff & EFF_LEDS) ledsApply();
    if (eff & EFF_DRV) displayReinit();
    if (eff & EFF_FLIP) displayApplyFlip();
  }
  // signed diff: correct across millis() overflow (49 days)
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    restartAt = 0;
    delay(150);
    ESP.restart();
  }
}
