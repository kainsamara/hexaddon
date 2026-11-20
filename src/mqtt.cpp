#include "mqtt.h"

#include "leds.h"
#include "net.h"
#include "settings.h"

#include <Update.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <espMqttClient.h>

// Synchronous MQTT client (bertmelis/espMqttClient): its network engine is
// pumped by client.loop() from our own mqttLoop() in the Arduino loop, so
// callbacks fire on loopTask — the same thread that owns settings, LEDs and
// the web server. No mutexes needed, no ISR-adjacent surprises.

static espMqttClient client;

// ---- topics: base = hostname, so multiple devices coexist on one broker ----

static String base() { return settings.hostname; }
static String tAvail() { return base() + "/avail"; }
static String tState() { return base() + "/light/state"; }
static String tCmd() { return base() + "/light/set"; }
static String tDiscovery() {
  return String("homeassistant/light/") + base() + "/light/config";
}
static String tAddState() { return base() + "/addon/state"; }
static String tAddCmd() { return base() + "/addon/set"; }
static String tSwitchDiscovery() {
  return String("homeassistant/switch/") + base() + "/addon/config";
}
// effect selector (HA select) + prev/next buttons: dashboard-visible
// switching without digging into the light's detail dialog
static String tEffectState() { return base() + "/effect/state"; }
static String tEffectSet() { return base() + "/effect/set"; }
static String tEffectNext() { return base() + "/effect/next"; }
static String tEffectPrev() { return base() + "/effect/prev"; }
static String tSelectDiscovery() {
  return String("homeassistant/select/") + base() + "/effect/config";
}
static String tBtnNextDiscovery() {
  return String("homeassistant/button/") + base() + "/effect_next/config";
}
static String tBtnPrevDiscovery() {
  return String("homeassistant/button/") + base() + "/effect_prev/config";
}
// speed slider (HA number, mode slider) and random-color switch
static String tSpeedState() { return base() + "/speed/state"; }
static String tSpeedSet() { return base() + "/speed/set"; }
static String tRndState() { return base() + "/color_random/state"; }
static String tRndSet() { return base() + "/color_random/set"; }
static String tNumberDiscovery() {
  return String("homeassistant/number/") + base() + "/speed/config";
}
static String tRndDiscovery() {
  return String("homeassistant/switch/") + base() + "/color_random/config";
}

// ---- publish helpers ----

// HA MQTT light, schema "json": {"state":"ON","brightness":1..255,"effect":..,
// "color_mode":"rgb","color":{"r":..,"g":..,"b":..}}. The color wheel lives
// in the light card (HA has no standalone color entity), the selected color
// tints Fire / seeds the random-hue effects.
static void publishState() {
  JsonDocument d;
  d["state"] = settings.animOn ? "ON" : "OFF";
  d["brightness"] = settings.brightness;
  d["effect"] = ledsAnimName(settings.anim);
  d["color_mode"] = "rgb";
  d["color"]["r"] = settings.colR;
  d["color"]["g"] = settings.colG;
  d["color"]["b"] = settings.colB;
  String out;
  serializeJson(d, out);
  client.publish(tState().c_str(), 0, true, out.c_str());
}

// both discoveries carry the same "dev" block → one HA device with two
// entities (light + switch)
static void addDevice(JsonDocument &d) {
  d["dev"]["ids"].add(base());
  d["dev"]["name"] = "HEXADDON";
  d["dev"]["mf"] = "Rotten Mechanism";
  d["dev"]["mdl"] = "HEXADDON v2";
  d["dev"]["sw"] = FW_VERSION;
}

static void publishAddonState() {
  client.publish(tAddState().c_str(), 0, true,
                 settings.addonPower ? "ON" : "OFF");
}

// name -> settings.anim (no-op for an unknown name). Returns true when the
// anim actually changed. Shared by the light JSON path and the effect
// select: the same match loop twice would drift apart on the first edit.
static bool applyEffectByName(const char *name) {
  for (int i = 0; i < NUM_ANIMS; i++) {
    if (strcmp(name, ledsAnimName(i)) == 0) {
      if ((uint8_t)i != settings.anim) {
        settings.anim = i;
        ledsClearFrame();  // same treatment an anim switch gets elsewhere
        return true;
      }
      return false;
    }
  }
  return false;
}

// plain current effect name for the HA select entity. The state payload
// must equal one of the discovery "options" — same strings, so it always
// validates.
static void publishEffectState() {
  client.publish(tEffectState().c_str(), 0, true,
                 ledsAnimName(settings.anim));
}

// bare number for the HA number (slider) entity — String outlives the call
static void publishSpeedState() {
  client.publish(tSpeedState().c_str(), 0, true,
                 String(settings.speed).c_str());
}

static void publishRndState() {
  client.publish(tRndState().c_str(), 0, true,
                 settings.colorRandom ? "ON" : "OFF");
}

static void publishDiscovery() {
  // The publish ORDER in this function is deliberate: HA registers
  // entities in the order the discovery messages arrive and lists them
  // that way on the device page / in the auto Entities card — MQTT
  // discovery has no other display-order knob. Panel order: addon power
  // FIRST (without the rail the backlight and every knob below it are
  // dead), then light, random color, effect select, prev, next, speed.
  // Registry order is sticky: a device that already exists in HA keeps
  // its old order until it is deleted there and the MQTT integration
  // is reloaded.

  // switch: Питание аддонов (the addon power rail, same target the button
  // and the scheduler use) — first: no power, no panel
  JsonDocument sw;
  sw["uniq_id"] = base() + "_addon";
  sw["name"] = "Питание аддонов";
  // static only: MQTT discovery has no icon_template (state-dependent
  // icons) yet — see AGENTS.md; swap to a template if HA ever ships it
  sw["icon"] = "mdi:hexagon-multiple";
  sw["command_topic"] = tAddCmd();
  sw["state_topic"] = tAddState();
  sw["availability_topic"] = tAvail();
  sw["payload_on"] = "ON";
  sw["payload_off"] = "OFF";
  sw["state_on"] = "ON";
  sw["state_off"] = "OFF";
  addDevice(sw);
  String swOut;
  serializeJson(sw, swOut);
  client.publish(tSwitchDiscovery().c_str(), 0, true, swOut.c_str());

  JsonDocument d;
  d["uniq_id"] = base() + "_light";
  d["name"] = "Подсветка";
  d["schema"] = "json";
  d["state_topic"] = tState();
  d["command_topic"] = tCmd();
  d["availability_topic"] = tAvail();
  d["brightness"] = true;
  d["color_mode"] = true;  // color wheel in the light card (rgb only)
  JsonArray modes = d["supported_color_modes"].to<JsonArray>();
  modes.add("rgb");
  d["effect"] = true;
  JsonArray eff = d["effect_list"].to<JsonArray>();
  for (int i = 0; i < NUM_ANIMS; i++) eff.add(ledsAnimName(i));
  addDevice(d);
  String out;
  serializeJson(d, out);
  // retained: HA picks the entity up even if it reboots after we do
  client.publish(tDiscovery().c_str(), 0, true, out.c_str());

  // switch: Случайный цвет — same shape as the addon power switch
  JsonDocument rc;
  rc["uniq_id"] = base() + "_color_random";
  rc["name"] = "Случайный цвет";
  rc["icon"] = "mdi:palette-outline";
  rc["command_topic"] = tRndSet();
  rc["state_topic"] = tRndState();
  rc["availability_topic"] = tAvail();
  rc["payload_on"] = "ON";
  rc["payload_off"] = "OFF";
  rc["state_on"] = "ON";
  rc["state_off"] = "OFF";
  addDevice(rc);
  String rcOut;
  serializeJson(rc, rcOut);
  client.publish(tRndDiscovery().c_str(), 0, true, rcOut.c_str());

  // select: Эффект — a dashboard-visible roller with every effect name
  // (the light card only shows its effect dropdown in the detail dialog)
  JsonDocument sel;
  sel["uniq_id"] = base() + "_effect";
  sel["name"] = "Эффект";
  JsonArray opts = sel["options"].to<JsonArray>();
  for (int i = 0; i < NUM_ANIMS; i++) opts.add(ledsAnimName(i));
  sel["command_topic"] = tEffectSet();
  sel["state_topic"] = tEffectState();
  sel["availability_topic"] = tAvail();
  addDevice(sel);
  String selOut;
  serializeJson(sel, selOut);
  client.publish(tSelectDiscovery().c_str(), 0, true, selOut.c_str());

  // buttons: Предыдущий/Следующий эффект — cyclic flipping without a name
  JsonDocument bp;
  bp["uniq_id"] = base() + "_effect_prev";
  bp["name"] = "Предыдущий эффект";
  bp["icon"] = "mdi:arrow-left-bold";
  bp["command_topic"] = tEffectPrev();
  bp["payload_press"] = "prev";
  bp["availability_topic"] = tAvail();
  addDevice(bp);
  String bpOut;
  serializeJson(bp, bpOut);
  client.publish(tBtnPrevDiscovery().c_str(), 0, true, bpOut.c_str());

  JsonDocument bn;
  bn["uniq_id"] = base() + "_effect_next";
  bn["name"] = "Следующий эффект";
  bn["icon"] = "mdi:arrow-right-bold";
  bn["command_topic"] = tEffectNext();
  bn["payload_press"] = "next";
  bn["availability_topic"] = tAvail();
  addDevice(bn);
  String bnOut;
  serializeJson(bn, bnOut);
  client.publish(tBtnNextDiscovery().c_str(), 0, true, bnOut.c_str());

  // number: Скорость — a real slider on the dashboard (mode slider)
  JsonDocument sp;
  sp["uniq_id"] = base() + "_speed";
  sp["name"] = "Скорость";
  sp["command_topic"] = tSpeedSet();
  sp["state_topic"] = tSpeedState();
  sp["min"] = 0;
  sp["max"] = 100;
  sp["step"] = 1;
  sp["mode"] = "slider";
  sp["availability_topic"] = tAvail();
  addDevice(sp);
  String spOut;
  serializeJson(sp, spOut);
  client.publish(tNumberDiscovery().c_str(), 0, true, spOut.c_str());
}

// ---- inbound commands (HA light JSON schema) ----
// Runs on loopTask inside client.loop(): mutating settings here is as safe
// as doing it from an HTTP handler.
static void onMessage(const espMqttClientTypes::MessageProperties &,
                      const char *topic, const uint8_t *payload, size_t len,
                      size_t index, size_t total) {
  // commands are tiny; if one ever arrives chunked, just wait for the last
  // chunk and parse the payload we have (index==0..total) — practically it
  // always arrives as a single frame
  if (index + len < total) return;

  bool isLight = (strcmp(topic, tCmd().c_str()) == 0);
  bool isAddon = (strcmp(topic, tAddCmd().c_str()) == 0);
  bool isEffSet = (strcmp(topic, tEffectSet().c_str()) == 0);
  bool isEffNext = (strcmp(topic, tEffectNext().c_str()) == 0);
  bool isEffPrev = (strcmp(topic, tEffectPrev().c_str()) == 0);
  bool isSpeedSet = (strcmp(topic, tSpeedSet().c_str()) == 0);
  bool isRndSet = (strcmp(topic, tRndSet().c_str()) == 0);
  if (!isLight && !isAddon && !isEffSet && !isEffNext && !isEffPrev &&
      !isSpeedSet && !isRndSet)
    return;

  // switch commands are plain "ON"/"OFF"; light commands are JSON
  if (isAddon) {
    String p((const char *)payload, len);
    p.trim();
    bool on = (p == "ON");
    if (on != settings.addonPower) {
      settings.addonPower = on;
      settings.applyPins();  // GPIO from loopTask, same as applyEffects would
      settings.save();
    }
    publishAddonState();
    return;
  }

  // speed slider (bare number). Strict parse: a garbage payload must not
  // silently zero the speed — strtol with an end-pointer check instead of
  // String::toInt() (which happily returns 0 for "abc").
  if (isSpeedSet) {
    String p((const char *)payload, len);
    p.trim();
    char *end = nullptr;
    long v = strtol(p.c_str(), &end, 10);
    if (end == p.c_str() || (end && *end)) {
      Serial.printf("[mqtt] bad speed payload: %s\n", p.c_str());
      publishSpeedState();  // snap the slider back to the real value
      return;
    }
    uint8_t s = (uint8_t)constrain((int)v, 0, 100);
    if (s != settings.speed) {
      settings.speed = s;
      settings.save();
    }
    publishSpeedState();
    return;
  }

  // random-color switch, same shape as the addon power switch
  if (isRndSet) {
    String p((const char *)payload, len);
    p.trim();
    bool on = (p == "ON");
    if (on != settings.colorRandom) {
      settings.colorRandom = on;
      settings.save();
    }
    publishRndState();
    return;
  }

  // effect select (payload = bare effect name) and prev/next buttons
  // (payload ignored). Plain strings, no JSON — easy to drive by hand.
  if (isEffSet || isEffNext || isEffPrev) {
    if (isEffSet) {
      String p((const char *)payload, len);
      p.trim();
      if (!applyEffectByName(p.c_str())) {
        if (p.length())
          Serial.printf("[mqtt] unknown effect: %s\n", p.c_str());
        publishEffectState();  // re-assert: a manual typo must not stick
        return;                // in the roller UI as a phantom state
      }
    } else {
      settings.anim = isEffNext
                          ? (uint8_t)((settings.anim + 1) % NUM_ANIMS)
                          : (uint8_t)((settings.anim + NUM_ANIMS - 1) % NUM_ANIMS);
      ledsClearFrame();
    }
    settings.save();     // deduped: an identical config never touches NVS
    publishState();      // light JSON carries the effect too
    publishEffectState();
    return;
  }

  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;

  bool dirty = false;
  if (d["state"].is<const char *>()) {
    bool on = (strcmp(d["state"].as<const char *>(), "ON") == 0);
    if (on != settings.animOn) {
      settings.animOn = on;
      dirty = true;
    }
  }
  if (d["brightness"].is<int>()) {
    // brightness is applied per-frame by ledsLoop() straight from settings
    uint8_t b = (uint8_t)constrain((int)d["brightness"], 0, 255);
    if (b != settings.brightness) {
      settings.brightness = b;
      dirty = true;
    }
  }
  if (d["effect"].is<const char *>())
    if (applyEffectByName(d["effect"].as<const char *>())) dirty = true;
  // color wheel in the light card: {"color":{"r":255,"g":128,"b":0}},
  // missing channels keep their current value (HA always sends all three)
  if (d["color"].is<JsonObjectConst>()) {
    JsonObjectConst c = d["color"];
    uint8_t r = (uint8_t)constrain((int)(c["r"] | (int)settings.colR), 0, 255);
    uint8_t g = (uint8_t)constrain((int)(c["g"] | (int)settings.colG), 0, 255);
    uint8_t b = (uint8_t)constrain((int)(c["b"] | (int)settings.colB), 0, 255);
    if (r != settings.colR || g != settings.colG || b != settings.colB) {
      settings.colR = r;
      settings.colG = g;
      settings.colB = b;
      dirty = true;
    }
  }
  if (!dirty) return;
  settings.save();       // deduped: an identical config never touches NVS
  publishState();        // answer immediately; optimistic=false in discovery
  publishEffectState();  // keep the select roller in sync too
}

// ---- connection lifecycle ----

static uint32_t retryDelayMs = 5000;
static uint32_t lastAttemptMs = 0;
static bool wantRestart = false;  // broker settings changed over HTTP
static uint32_t discPumpUntil = 0;  // client.loop() deadline of the DISCONNECT flush

// espMqttClient keeps raw pointers to every string passed to its setters
// (setWill stores the topic pointer as-is, it is only READ when connect()
// builds the CONNECT packet), so the client id AND the will topic must
// live in stable buffers, not temporary Strings: tAvail() returns String
// by value and its heap buffer is freed long before connect() runs —
// a use-after-free that corrupts the LWT topic at random.
// (settings.* fields are stable: mutated only by applyConfig/load on the
// same thread, and re-fed to the setters right before every connect()).
static String clientIdBuf;
static String willTopicBuf;

static void applyBrokerConfig() {
  clientIdBuf = settings.hostname + "-light";
  willTopicBuf = tAvail();
  client.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  client.setClientId(clientIdBuf.c_str());
  client.setCredentials(settings.mqttUser.c_str(), settings.mqttPass.c_str());
  client.setWill(willTopicBuf.c_str(), 1, true, "offline");
}

static void onConnect(uint8_t) {
  Serial.printf("[mqtt] connected to %s:%u as %s\n", settings.mqttHost.c_str(),
                (unsigned)settings.mqttPort, settings.mqttUser.c_str());
  retryDelayMs = 5000;
  client.publish(tAvail().c_str(), 0, true, "online");
  publishDiscovery();
  // same order as publishDiscovery(): the code reads like the HA panel
  client.subscribe(tAddCmd().c_str(), 0);
  client.subscribe(tCmd().c_str(), 0);
  client.subscribe(tRndSet().c_str(), 0);
  client.subscribe(tEffectSet().c_str(), 0);
  client.subscribe(tEffectPrev().c_str(), 0);
  client.subscribe(tEffectNext().c_str(), 0);
  client.subscribe(tSpeedSet().c_str(), 0);
  publishAddonState();
  publishState();
  publishRndState();
  publishEffectState();
  publishSpeedState();
}

static void onDisconnect(espMqttClientTypes::DisconnectReason reason) {
  Serial.printf("[mqtt] disconnected, reason=%d\n", (int)reason);
}

void mqttRestart() { wantRestart = true; }

void mqttBegin() {
  client.onConnect(onConnect);
  client.onDisconnect(onDisconnect);
  client.onMessage(onMessage);
}

// ---- state snapshot for the change detector ----
// Local changes (button, scheduler, web UI) never notify us, so instead of
// scattering hooks we compare at 1 Hz what we last published against the
// live settings and re-publish on any drift. Plus a 30 s heartbeat.
struct Snap {
  bool on;
  bool addon;
  bool rnd;
  uint8_t bri, anim, speed, r, g, b;
};
static Snap lastSnap{false, false, false, 0, 0, 0, 0, 0, 0};
static bool snapValid = false;
static uint32_t lastCheckMs = 0;
static uint32_t lastBeatMs = 0;

void mqttLoop() {
  // portal / disabled / OTA flash in progress: keep hands off the radio.
  // Update.h is not thread safe and the OTA task owns the flash meanwhile.
  if (!settings.mqttEnable || netIsPortal() || Update.isRunning()) {
    if (client.connected()) {
      // espMqttClient builds the DISCONNECT packet in disconnect() but only
      // SENDS queued packets from inside client.loop() — requesting one and
      // walking away leaves it unsent, the TCP lingering and the broker
      // learning of our death from its keepalive timer instead (LWT fires
      // late). Pump the engine for a short grace window so the packet
      // leaves and the socket closes cleanly.
      if (!discPumpUntil) {
        client.disconnect();
        snapValid = false;
        uint32_t d = millis() + 1000;
        discPumpUntil = d ? d : 1;  // 0 means "idle" (sensor.cpp deadline style)
      }
      if ((int32_t)(millis() - discPumpUntil) < 0) {
        client.loop();  // the DISCONNECT flushes from here
        return;
      }
    } else {
      discPumpUntil = 0;  // socket gone: re-arm for the next shutdown
    }
    snapValid = false;
    return;
  }
  discPumpUntil = 0;  // normal operation: nothing pending
  if (WiFi.status() != WL_CONNECTED) {
    // no radio, but still pump the client's engine: espMqttClient only
    // notices a dead TCP socket from inside loop(), and without it the
    // disconnect callbacks (and a clean reconnect later) stall until the
    // radio returns on its own
    client.loop();
    return;
  }

  if (wantRestart) {
    wantRestart = false;
    if (client.connected()) client.disconnect();
    snapValid = false;
    lastAttemptMs = 0;  // reconnect right away with the fresh settings
    retryDelayMs = 5000;
  }

  if (!client.connected()) {
    uint32_t now = millis();
    if (now - lastAttemptMs < retryDelayMs) {
      client.loop();
      return;
    }
    lastAttemptMs = now;
    retryDelayMs = min(retryDelayMs * 2, 60000UL);  // 5s → 60s backoff
    applyBrokerConfig();
    client.connect();
    client.loop();
    return;
  }

  client.loop();

  uint32_t now = millis();
  if (now - lastCheckMs < 1000) return;
  lastCheckMs = now;
  Snap s{settings.animOn, settings.addonPower, settings.colorRandom,
         settings.brightness, settings.anim, settings.speed,
         settings.colR, settings.colG, settings.colB};
  bool drift =
      !snapValid || s.on != lastSnap.on || s.bri != lastSnap.bri ||
      s.anim != lastSnap.anim || s.addon != lastSnap.addon ||
      s.rnd != lastSnap.rnd || s.speed != lastSnap.speed ||
      s.r != lastSnap.r || s.g != lastSnap.g || s.b != lastSnap.b;
  if (drift) {
    // only the topic that actually changed needs a re-publish; two small
    // packets beat one combined topic that both entities have to parse
    bool lightChanged =
        !snapValid || s.on != lastSnap.on || s.bri != lastSnap.bri ||
        s.anim != lastSnap.anim || s.r != lastSnap.r || s.g != lastSnap.g ||
        s.b != lastSnap.b;
    bool animChanged = !snapValid || s.anim != lastSnap.anim;
    bool addonChanged = !snapValid || s.addon != lastSnap.addon;
    bool speedChanged = !snapValid || s.speed != lastSnap.speed;
    bool rndChanged = !snapValid || s.rnd != lastSnap.rnd;
    lastSnap = s;
    snapValid = true;
    lastBeatMs = now;
    if (lightChanged) publishState();  // carries color + brightness too
    if (animChanged) publishEffectState();  // the select roller follows too
    if (addonChanged) publishAddonState();
    if (speedChanged) publishSpeedState();
    if (rndChanged) publishRndState();
  } else if (now - lastBeatMs >= 30000UL) {
    lastBeatMs = now;
    publishState();
    publishAddonState();
    publishEffectState();
    publishSpeedState();
    publishRndState();
  }
}

const char *mqttStateStr() {
  if (!settings.mqttEnable) return "off";
  return client.connected() ? "online" : "offline";
}
