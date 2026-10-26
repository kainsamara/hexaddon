#include "web.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "leds.h"
#include "display.h"
#include "net.h"
#include "ota.h"
#include "sensor.h"
#include "settings.h"
#include "index_html.h"  // generated from data/index.html by scripts/embed_index.py
#include "favicon_svg.h"  // generated from data/favicon.svg by scripts/embed_index.py

static WebServer server(80);
static uint32_t restartAt = 0;
static uint8_t imgBuf[IMG_SIZE];

static void scheduleRestart(uint32_t delayMs = 500) {
  restartAt = millis() + delayMs;
}

static void sendJson(int code, const String &s) {
  server.send(code, "application/json", s);
}

static void sendErr(int code, const char *msg) {
  String s = "{\"error\":\"";
  s += msg;
  s += "\"}";
  sendJson(code, s);
}

// WebServer buffers the whole POST body in RAM before the handler runs;
// refuse oversized payloads before we parse them (image posts are ~2KB hex).
static bool bodyTooLarge() {
  if (server.clientContentLength() > 8192) {
    sendErr(413, "too large");
    return true;
  }
  return false;
}

// ---------------- hex helpers (WebServer plain arg stops at \0, so no raw binary) ----------------

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

// ---------------- json ----------------

static void fillStatus(JsonDocument &d) {
  d["ok"] = true;
  d["version"] = FW_VERSION;
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
  d["hostname"] = settings.hostname;
  d["tz"] = settings.tz;
  d["theme"] = settings.theme;
}

static String docStr(JsonDocument &d) {
  String out;
  serializeJson(d, out);
  return out;
}

static void handleStatus() {
  JsonDocument d;
  fillStatus(d);
  sendJson(200, docStr(d));
}

static void handleConfigGet() {
  JsonDocument d;
  fillConfig(d);
  sendJson(200, docStr(d));
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
  if (d["addonPower"].is<bool>()) s.addonPower = d["addonPower"].as<bool>();

  if (d["gpio0"].is<JsonObjectConst>()) {
    JsonObjectConst g = d["gpio0"];
    if (g["mode"].is<const char *>()) s.gpio0.mode = gpioModeFromStr(g["mode"].as<String>());
    if (g["value"].is<int>()) s.gpio0.value = g["value"] ? 1 : 0;
  }
  if (d["gpio1"].is<JsonObjectConst>()) {
    JsonObjectConst g = d["gpio1"];
    if (g["mode"].is<const char *>()) s.gpio1.mode = gpioModeFromStr(g["mode"].as<String>());
    if (g["value"].is<int>()) s.gpio1.value = g["value"] ? 1 : 0;
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
  if (drvChanged) displayReinit();

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
    if (changed) displayApplyFlip();
  }
  if (d["tempOffset"].is<float>() || d["tempOffset"].is<int>())
    s.tempOffset = constrain((float)d["tempOffset"].as<float>(), -15.0f, 15.0f);
  if (d["tz"].is<const char *>()) s.tz = d["tz"].as<String>();  // sanitized + applied below
  if (d["theme"].is<const char *>()) s.theme = d["theme"].as<String>();

  if (d["hostname"].is<const char *>()) {
    String old = s.hostname;
    s.hostname = d["hostname"].as<String>();
    s.sanitize();  // compare normalized values: cosmetic edits (case,
                   // bad chars) must not force a pointless reboot
    if (s.hostname != old) reboot = true;
  }

  s.sanitize();
  s.save();
  s.applyPins();
  ledsApply();
  return reboot;
}

static void handleConfigPost() {
  if (bodyTooLarge()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "bad json");
    return;
  }
  bool reboot = applyConfig(d);
  JsonDocument r;
  fillConfig(r);
  if (reboot) r["reboot"] = true;
  sendJson(200, docStr(r));
  if (reboot) scheduleRestart();
}

// ---------------- wifi ----------------

// Async scan: the first request starts it, polling requests collect the
// result. A synchronous scanNetworks() freezes the whole loop (LEDs,
// button, web) for 2-4 seconds while it sweeps the channels.
static bool scanStarted = false;

static void handleNetworks() {
  if (!scanStarted) {
    WiFi.scanNetworks(true);
    scanStarted = true;
    sendJson(202, "{\"scanning\":true}");
    return;
  }
  int st = WiFi.scanComplete();
  if (st == WIFI_SCAN_RUNNING) {
    sendJson(202, "{\"scanning\":true}");
    return;
  }
  if (st < 0) {  // failed / not reported: restart on the next request
    scanStarted = false;
    sendJson(202, "{\"scanning\":true}");
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
  sendJson(200, docStr(d));
}

static void handleWifiPost() {
  if (bodyTooLarge()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "bad json");
    return;
  }
  String ssid = d["ssid"] | "";
  String pass = d["pass"] | "";
  if (!ssid.length()) {
    sendErr(400, "empty ssid");
    return;
  }
  if (ssid.length() > 32 || pass.length() > 64) {
    sendErr(400, "ssid/pass too long");
    return;
  }
  settings.setWifi(ssid, pass);
  sendJson(200, "{\"ok\":true,\"msg\":\"Сохранено, перезагрузка...\"}");
  scheduleRestart(700);
}

static void handleWifiForget() {
  settings.clearWifi();
  sendJson(200, "{\"ok\":true,\"msg\":\"Сеть забыта\"}");
  scheduleRestart(700);
}

// ---------------- display image ----------------

static void handleImageGet() {
  uint8_t addr;
  if (!parseAddr(server.arg("addr"), addr)) {
    sendErr(400, "addr=0x3C|0x3D");
    return;
  }
  uint8_t buf[IMG_SIZE];
  ::memset(buf, 0, IMG_SIZE);
  bool exists = displayHasImage(addr);
  if (exists) displayCopyImage(addr, buf);
  JsonDocument d;
  d["addr"] = addrStr(addr);
  d["exists"] = exists;
  d["size"] = IMG_SIZE;
  d["data"] = toHex(buf, IMG_SIZE);
  sendJson(200, docStr(d));
}

static void handleImagePost() {
  if (bodyTooLarge()) return;
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "bad json");
    return;
  }
  uint8_t addr;
  if (!parseAddr(d["addr"] | "", addr)) {
    sendErr(400, "addr=0x3C|0x3D");
    return;
  }
  String data = d["data"] | "";
  if (!fromHex(data, imgBuf, IMG_SIZE)) {
    sendErr(400, "need 1024 bytes hex");
    return;
  }
  if (!displaySaveImage(addr, imgBuf, IMG_SIZE)) {
    sendErr(500, "save failed");
    return;
  }
  displayToast("Image " + addrStr(addr));
  sendJson(200, "{\"ok\":true}");
}

static void handleImageDelete() {
  uint8_t addr;
  if (!parseAddr(server.arg("addr"), addr)) {
    sendErr(400, "addr=0x3C|0x3D");
    return;
  }
  displayClearImage(addr);
  displayToast("Deleted " + addrStr(addr));
  sendJson(200, "{\"ok\":true}");
}

// ---------------- system ----------------

static void handleReboot() {
  sendJson(200, "{\"ok\":true}");
  scheduleRestart(600);
}

// ---------------- OTA firmware update ----------------
// The min_spiffs.csv partition table already ships two 1.9MB OTA slots;
// Update.begin() writes into the inactive one, the bootloader swaps on
// reboot. The web UI streams the .bin here as multipart/form-data.

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] start: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      Serial.printf("[ota] done: %u bytes\n", (unsigned)up.totalSize);
    else
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end(false);
    Serial.println("[ota] aborted");
  }
}

static void handleUpdatePost() {
  if (Update.hasError() || !Update.isFinished()) {
    sendErr(500, Update.errorString());
    return;
  }
  sendJson(200, "{\"ok\":true}");
  scheduleRestart(600);
}

// ---------------- OTA from the GitHub release feed ----------------
// The device itself queries/downloads from GitHub in background tasks (a
// synchronous TLS request would freeze the whole loop, see /api/networks).

static void handleUpdateCheck() {
  const OtaState &s = otaState();
  if (s.checking || !s.checked || millis() - s.checkedAt > CHECK_TTL_MS) {
    if (!s.checking) otaCheckAsync();
    sendJson(202, "{\"checking\":true}");
    return;
  }
  if (s.netError) {
    sendJson(200, "{\"current\":\"" + String(FW_VERSION) + "\",\"error\":\"check failed\"}");
    return;
  }
  JsonDocument d;
  d["current"] = FW_VERSION;
  d["latest"] = s.latest;
  d["available"] = otaNewerThan(s.latest);
  sendJson(200, docStr(d));
}

static void handleOtaStart() {
  String err;
  if (!otaStart(err)) {
    sendErr(409, err.c_str());
    return;
  }
  sendJson(202, "{\"ok\":true}");
}

static void handleOtaStatus() {
  const OtaState &s = otaState();
  JsonDocument d;
  d["state"] = s.stage;
  d["progress"] = s.progress;
  if (s.error.length()) d["error"] = s.error;
  sendJson(200, docStr(d));
}

static void handleFactory() {
  settings.factoryReset();
  // RAM still holds the old config — a settings.save() in the 800ms
  // restart window (button action, /api/config) would write it back
  settings = Settings();
  settings.applyPins();
  LittleFS.remove("/img_3c.xbm");  // legacy images (pre-NVS firmware)
  LittleFS.remove("/img_3d.xbm");
  sendJson(200, "{\"ok\":true}");
  scheduleRestart(800);
}

// ---------------- static / not found ----------------

static void serveIndex() {
  if (INDEX_HTML_GZ_LEN) {
    // embedded (gzip) page: OTA updates it together with the firmware
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html; charset=utf-8",
                  (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    return;
  }
  // fallback for a hand-built firmware without the embedded page
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain; charset=utf-8", "index.html is not built in");
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

static void serveFavicon() {
  if (!FAVICON_SVG_LEN) {  // data/favicon.svg was missing at build time
    sendErr(404, "no favicon");
    return;
  }
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send_P(200, "image/svg+xml", (const char *)FAVICON_SVG, FAVICON_SVG_LEN);
}

static void handleNotFound() {
  String uri = server.uri();
  if (netIsPortal()) {
    // captive portal: redirect probes and everything else to the setup page
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  if (server.method() == HTTP_GET && !uri.startsWith("/api/")) {
    serveIndex();
    return;
  }
  sendErr(404, "not found");
}

void webBegin() {
  server.on("/", HTTP_GET, serveIndex);
  server.on("/favicon.svg", HTTP_GET, serveFavicon);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/networks", HTTP_GET, handleNetworks);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/display/image", HTTP_GET, handleImageGet);
  server.on("/api/display/image", HTTP_POST, handleImagePost);
  server.on("/api/display/image", HTTP_DELETE, handleImageDelete);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.on("/api/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  server.on("/api/update/check", HTTP_GET, handleUpdateCheck);
  server.on("/api/update/ota", HTTP_GET, handleOtaStatus);
  server.on("/api/update/ota", HTTP_POST, handleOtaStart);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
  // signed diff: correct across millis() overflow (49 days)
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    restartAt = 0;
    delay(150);
    ESP.restart();
  }
}
