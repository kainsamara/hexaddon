#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

Settings settings;

static const char *ORDER_LIST[] = {"GRB", "RGB", "BRG", "BGR", "RBG", "GBR"};

// valid values for settings.tz (POSIX TZ strings; the sign is inverted by
// the standard: "UTC-3" means UTC+3). Must stay in sync with TZ_LIST in
// data/index.html: sanitize() resets anything else to UTC-3 (= Moscow).
static const char *TZ_LIST[] = {
    "UTC0",
    "UTC-1", "UTC-2", "UTC-3", "UTC-4", "UTC-5", "UTC-6",
    "UTC-7", "UTC-8", "UTC-9", "UTC-10", "UTC-11", "UTC-12",
};

const char *gpioModeStr(GpioMode m) {
  switch (m) {
    case GpioMode::PullUp: return "pullup";
    case GpioMode::Output: return "output";
    default: return "input";
  }
}

GpioMode gpioModeFromStr(const String &s) {
  if (s == "pullup") return GpioMode::PullUp;
  if (s == "output") return GpioMode::Output;
  return GpioMode::Input;
}

const char *dispModeStr(DispMode m) {
  switch (m) {
    case DispMode::Image: return "image";
	case DispMode::Anim: return "anim";
    case DispMode::Off: return "off";
    case DispMode::Sensors: return "sensors";
    case DispMode::Clock: return "clock";
    default: return "info";
  }
}

DispMode dispModeFromStr(const String &s) {
  if (s == "image") return DispMode::Image;
    if (s == "anim") return DispMode::Anim;
  if (s == "off") return DispMode::Off;
  if (s == "sensors") return DispMode::Sensors;
  if (s == "clock") return DispMode::Clock;
  return DispMode::Info;
}

const char *btnActionStr(uint8_t a) {
  switch (a) {
    case BTN_POWER: return "power";
    case BTN_DISPLAY0: return "display0";
    case BTN_DISPLAY1: return "display1";
    case BTN_GPIO1: return "gpio1";
    case BTN_GPIO2: return "gpio2";
    case BTN_REBOOT: return "reboot";
    case BTN_WIFI: return "wifireset";
    default: return "light";
  }
}

uint8_t btnActionFromStr(const String &s, uint8_t def) {
  if (s == "light") return BTN_LIGHT;
  if (s == "power") return BTN_POWER;
  // legacy "display" cycled both displays; map it to the 0x3C action
  if (s == "display" || s == "display0") return BTN_DISPLAY0;
  if (s == "display1") return BTN_DISPLAY1;
  if (s == "gpio1") return BTN_GPIO1;
  if (s == "gpio2") return BTN_GPIO2;
  if (s == "reboot") return BTN_REBOOT;
  if (s == "wifireset") return BTN_WIFI;
  return def;
}

String Settings::colorHex() const {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", colR, colG, colB);
  return String(buf);
}

// Shared with web.cpp: the "did the hostname change -> reboot" decision is
// computed in the HTTP handler (async_tcp) and must match sanitize() exactly
String sanitizeHostname(const String &in) {
  String s = in;
  s.trim();
  s.toLowerCase();
  String clean;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-') clean += c;
  }
  if (clean.length() > 32) clean = clean.substring(0, 32);
  if (clean.length() == 0) clean = "hexaddon";
  return clean;
}

// __DATE__ is "Mmm dd yyyy" (the day is space-padded), __TIME__ "hh:mm:ss" -
// rearrange into "27 Oct 2026 18:32"
String buildStamp() {
  String d = __DATE__, t = __TIME__;
  String day = d.substring(4, 6);
  day.trim();
  return day + ' ' + d.substring(0, 3) + ' ' + d.substring(7, 11) + ' ' +
         t.substring(0, 5);
}

void Settings::setColorHex(const String &hex) {
  String s = hex;
  s.trim();
  if (s.startsWith("#")) s = s.substring(1);
  if (s.length() != 6) return;
  colR = strtoul(s.substring(0, 2).c_str(), nullptr, 16);
  colG = strtoul(s.substring(2, 4).c_str(), nullptr, 16);
  colB = strtoul(s.substring(4, 6).c_str(), nullptr, 16);
}

void Settings::sanitize() {
  if (panels < 1) panels = 1;
  if (panels > MAX_PANELS) panels = MAX_PANELS;
  if (anim >= NUM_ANIMS) anim = 0;
  if (speed > 100) speed = 100;
  if (powerMa > 20000) powerMa = 20000;
  if (tempOffset < -15.0f) tempOffset = -15.0f;
  if (tempOffset > 15.0f) tempOffset = 15.0f;
  tempOffset = roundf(tempOffset * 2.0f) / 2.0f;

  // sensor history step: fixed set (the UI select mirrors it exactly)
  switch (histStepMin) {
    case 1: case 2: case 5: case 10: case 15: case 30: break;
    default: histStepMin = 5;
  }

  for (int i = 0; i < MAX_SCHED_RULES; i++) {
    SchedRule &r = sched[i];
    r.days &= 0x7F;
    if (r.days == 0) r.days = 0x7F;
    if (r.hour > 23) r.hour = 23;
    if (r.minute > 59) r.minute = 59;
    if (r.durationMin > 1440) r.durationMin = 1440;
    if (r.target > 1) r.target = 0;
  }

  bool knownOrder = false;
  for (const char *o : ORDER_LIST) {
    if (order == o) { knownOrder = true; break; }
  }
  if (!knownOrder) order = "GRB";

  if (driver[0] != "sh1106") driver[0] = "ssd1306";
  if (driver[1] != "sh1106") driver[1] = "ssd1306";

  static const uint8_t BTN_DEF[BTN_SLOTS] = {BTN_LIGHT, BTN_POWER, BTN_REBOOT,
                                             BTN_WIFI};
  for (int i = 0; i < BTN_SLOTS; i++)
    if (btnAct[i] >= BTN_ACTIONS) btnAct[i] = BTN_DEF[i];

  hostname = sanitizeHostname(hostname);

  bool tzOk = false;
  for (const char *z : TZ_LIST) {
    if (tz == z) { tzOk = true; break; }
  }
  // legacy saved values used the "MSK-x" spelling — remap, don't reset
  if (!tzOk && tz.startsWith("MSK-")) {
    String remap = "UTC-" + tz.substring(4);
    for (const char *z : TZ_LIST) {
      if (remap == z) { tz = remap; tzOk = true; break; }
    }
  }
  if (!tzOk) tz = "UTC-3";
  // apply right away: load() and applyConfig() both run sanitize()
  setenv("TZ", tz.c_str(), 1);
  tzset();

  if (theme != "light") theme = "dark";
}

static String lastCfgJson;  // avoid rewriting NVS when nothing changed

static void cfgToJson(JsonDocument &d) {
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
  JsonArray da = d["drivers"].to<JsonArray>();
  da.add(settings.driver[0]);
  da.add(settings.driver[1]);
  JsonArray fa = d["flips"].to<JsonArray>();
  fa.add(settings.flip[0]);
  fa.add(settings.flip[1]);
  JsonArray apa = d["animPresets"].to<JsonArray>();
  apa.add(animPresetStr(settings.animPreset[0]));
  apa.add(animPresetStr(settings.animPreset[1]));
  JsonArray ba = d["btns"].to<JsonArray>();
  for (int i = 0; i < BTN_SLOTS; i++) ba.add(btnActionStr(settings.btnAct[i]));
  d["tempOffset"] = settings.tempOffset;
  d["histStepMin"] = settings.histStepMin;
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

void Settings::save() {
  JsonDocument d;
  cfgToJson(d);
  String out;
  serializeJson(d, out);
  if (out == lastCfgJson) return;  // identical config: spare NVS wear
  Preferences p;
  p.begin("hexaddon", false);
  p.putString("cfg", out);
  p.end();
  lastCfgJson = out;
}

void Settings::load() {
  Preferences p;
  p.begin("hexaddon", true);
  String cfg = p.getString("cfg", "");
  wifiSsid = p.getString("ssid", "");
  wifiPass = p.getString("pass", "");
  p.end();
  wifiConfigured = wifiSsid.length() > 0;

  if (cfg.length()) {
    JsonDocument d;
    if (!deserializeJson(d, cfg)) {
      panels = d["panels"] | panels;
      reverse = d["reverse"] | reverse;
      brightness = d["brightness"] | brightness;
      speed = d["speed"] | speed;
      anim = d["anim"] | anim;
      animOn = d["animOn"] | animOn;
      if (d["scope"].is<const char *>())
        scope = (String(d["scope"].as<const char *>()) == "chain") ? Scope::Chain
                                                                   : Scope::Panel;
      if (d["color"].is<const char *>()) setColorHex(d["color"].as<String>());
      colorRandom = d["colorRandom"] | colorRandom;
      if (d["order"].is<const char *>()) order = d["order"].as<String>();
      powerMa = d["powerMa"] | powerMa;
      addonPower = d["addonPower"] | addonPower;
      if (d["gpio0"].is<JsonObjectConst>()) {
        gpio0.mode = gpioModeFromStr(String(d["gpio0"]["mode"] | "input"));
        gpio0.value = d["gpio0"]["value"] | 0;
      }
      if (d["gpio1"].is<JsonObjectConst>()) {
        gpio1.mode = gpioModeFromStr(String(d["gpio1"]["mode"] | "input"));
        gpio1.value = d["gpio1"]["value"] | 0;
      }
      // legacy scalar keys applied to both displays, then per-display arrays
      if (d["displayMode"].is<const char *>()) {
        DispMode m = dispModeFromStr(String(d["displayMode"].as<const char *>()));
        displayMode[0] = displayMode[1] = m;
      }
      if (d["displayModes"].is<JsonArray>()) {
        JsonArray ma = d["displayModes"].as<JsonArray>();
        if (ma.size() > 0) displayMode[0] = dispModeFromStr(String(ma[0] | "info"));
        if (ma.size() > 1) displayMode[1] = dispModeFromStr(String(ma[1] | "info"));
      }
      if (d["driver"].is<const char *>()) {
        String drv = d["driver"].as<String>();
        driver[0] = driver[1] = drv;
      }
      if (d["drivers"].is<JsonArray>()) {
        JsonArray da = d["drivers"].as<JsonArray>();
        if (da.size() > 0) driver[0] = String(da[0] | "ssd1306");
        if (da.size() > 1) driver[1] = String(da[1] | "ssd1306");
      }
      if (d["flips"].is<JsonArray>()) {
        JsonArray fa = d["flips"].as<JsonArray>();
        if (fa.size() > 0) flip[0] = fa[0] | false;
        if (fa.size() > 1) flip[1] = fa[1] | false;
      }
	  if (d["animPresets"].is<JsonArray>()) {
        JsonArray apa = d["animPresets"].as<JsonArray>();
        if (apa.size() > 0) animPreset[0] = animPresetFromStr(String(apa[0] | "offzone2024"));
        if (apa.size() > 1) animPreset[1] = animPresetFromStr(String(apa[1] | "offzone2024"));
      }
      if (d["btns"].is<JsonArray>()) {
        JsonArray ba = d["btns"].as<JsonArray>();
        for (int i = 0; i < BTN_SLOTS && i < (int)ba.size(); i++)
          btnAct[i] = btnActionFromStr(String(ba[i] | ""), btnAct[i]);
      }
      tempOffset = d["tempOffset"] | tempOffset;
      histStepMin = d["histStepMin"] | histStepMin;
      if (d["sched"].is<JsonArray>()) {
        JsonArray sa = d["sched"].as<JsonArray>();
        for (int i = 0; i < MAX_SCHED_RULES && i < (int)sa.size(); i++) {
          JsonObjectConst o = sa[i];
          if (o.isNull()) continue;
          SchedRule &r = sched[i];
          r.used = true;
          r.on = o["on"] | r.on;
          r.days = (uint8_t)(o["days"] | r.days);
          r.hour = (uint8_t)(o["h"] | r.hour);
          r.minute = (uint8_t)(o["m"] | r.minute);
          r.enable = o["act"] | r.enable;
          r.durationMin = (uint16_t)(o["dur"] | r.durationMin);
          r.target = String(o["tgt"] | "light") == "power" ? 1 : 0;
        }
      }
      if (d["hostname"].is<const char *>()) hostname = d["hostname"].as<String>();
      if (d["tz"].is<const char *>()) tz = d["tz"].as<String>();
      if (d["theme"].is<const char *>()) theme = d["theme"].as<String>();
    }
  }
  sanitize();
  // seed the save-dedup: an unchanged config must not rewrite NVS after boot
  {
    JsonDocument d;
    cfgToJson(d);
    serializeJson(d, lastCfgJson);
  }
}

void Settings::setWifi(const String &ssid, const String &pass) {
  wifiSsid = ssid;
  wifiPass = pass;
  wifiConfigured = ssid.length() > 0;
  Preferences p;
  p.begin("hexaddon", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

void Settings::clearWifi() {
  wifiSsid = "";
  wifiPass = "";
  wifiConfigured = false;
  Preferences p;
  p.begin("hexaddon", false);
  p.remove("ssid");
  p.remove("pass");
  p.end();
}

void Settings::factoryReset() {
  lastCfgJson = "";
  Preferences p;
  p.begin("hexaddon", false);
  p.clear();
  p.end();
}

static void applyGpio(uint8_t pin, const GpioCfg &g) {
  switch (g.mode) {
    case GpioMode::Output:
      pinMode(pin, OUTPUT);
      digitalWrite(pin, g.value ? HIGH : LOW);
      break;
    case GpioMode::PullUp:
      pinMode(pin, INPUT_PULLUP);
      break;
    default:
      pinMode(pin, INPUT);
      break;
  }
}

void Settings::applyPins() {
  pinMode(PIN_PWR, OUTPUT);
  digitalWrite(PIN_PWR, addonPower ? HIGH : LOW);
  applyGpio(PIN_IO0, gpio0);
  applyGpio(PIN_IO1, gpio1);
}

const char *animPresetStr(AnimPreset p) {
  if (p == AnimPreset::Addon2022) return "addon2022";
  return (p == AnimPreset::Offzone2026) ? "offzone2026" : "offzone2024";
}

AnimPreset animPresetFromStr(const String &s) {
  if (s == "addon2022") return AnimPreset::Addon2022;
  if (s == "offzone2026") return AnimPreset::Offzone2026;
  return AnimPreset::Offzone2024;
}