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

// ---- publish helpers ----

// HA MQTT light, schema "json": {"state":"ON","brightness":1..255,"effect":..}
static void publishState() {
  JsonDocument d;
  d["state"] = settings.animOn ? "ON" : "OFF";
  d["brightness"] = settings.brightness;
  d["effect"] = ledsAnimName(settings.anim);
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

static void publishDiscovery() {
  JsonDocument d;
  d["uniq_id"] = base() + "_light";
  d["name"] = "Подсветка";
  d["schema"] = "json";
  d["state_topic"] = tState();
  d["command_topic"] = tCmd();
  d["availability_topic"] = tAvail();
  d["brightness"] = true;
  d["effect"] = true;
  JsonArray eff = d["effect_list"].to<JsonArray>();
  for (int i = 0; i < NUM_ANIMS; i++) eff.add(ledsAnimName(i));
  addDevice(d);
  String out;
  serializeJson(d, out);
  // retained: HA picks the entity up even if it reboots after we do
  client.publish(tDiscovery().c_str(), 0, true, out.c_str());

  // switch: Питание аддонов (the addon power rail, same target the button
  // and the scheduler use)
  JsonDocument sw;
  sw["uniq_id"] = base() + "_addon";
  sw["name"] = "Питание аддонов";
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
  if (!isLight && !isAddon) return;

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
  if (d["effect"].is<const char *>()) {
    const char *e = d["effect"].as<const char *>();
    for (int i = 0; i < NUM_ANIMS; i++) {
      if (strcmp(e, ledsAnimName(i)) == 0) {
        if ((uint8_t)i != settings.anim) {
          settings.anim = i;
          ledsClearFrame();  // same treatment an anim switch gets elsewhere
          dirty = true;
        }
        break;
      }
    }
  }
  if (!dirty) return;
  settings.save();       // deduped: an identical config never touches NVS
  publishState();        // answer immediately; optimistic=false in discovery
}

// ---- connection lifecycle ----

static uint32_t retryDelayMs = 5000;
static uint32_t lastAttemptMs = 0;
static bool wantRestart = false;  // broker settings changed over HTTP

// espMqttClient keeps raw pointers to every string passed to its setters,
// so the client id must live in a stable buffer, not a temporary String
// (settings.* fields are stable: mutated only by applyConfig/load on the
// same thread, and re-fed to the setters right before every connect()).
static String clientIdBuf;

static void applyBrokerConfig() {
  clientIdBuf = settings.hostname + "-light";
  client.setServer(settings.mqttHost.c_str(), settings.mqttPort);
  client.setClientId(clientIdBuf.c_str());
  client.setCredentials(settings.mqttUser.c_str(), settings.mqttPass.c_str());
  client.setWill(tAvail().c_str(), 1, true, "offline");
}

static void onConnect(uint8_t) {
  Serial.printf("[mqtt] connected to %s:%u as %s\n", settings.mqttHost.c_str(),
                (unsigned)settings.mqttPort, settings.mqttUser.c_str());
  retryDelayMs = 5000;
  client.publish(tAvail().c_str(), 0, true, "online");
  publishDiscovery();
  client.subscribe(tCmd().c_str(), 0);
  client.subscribe(tAddCmd().c_str(), 0);
  publishState();
  publishAddonState();
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
  uint8_t bri, anim;
};
static Snap lastSnap{false, false, 0, 0};
static bool snapValid = false;
static uint32_t lastCheckMs = 0;
static uint32_t lastBeatMs = 0;

void mqttLoop() {
  // portal / disabled / OTA flash in progress: keep hands off the radio.
  // Update.h is not thread safe and the OTA task owns the flash meanwhile.
  if (!settings.mqttEnable || netIsPortal() || Update.isRunning()) {
    if (client.connected()) client.disconnect();
    snapValid = false;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;

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
  Snap s{settings.animOn, settings.addonPower, settings.brightness,
         settings.anim};
  bool drift = !snapValid || s.on != lastSnap.on || s.bri != lastSnap.bri ||
               s.anim != lastSnap.anim || s.addon != lastSnap.addon;
  if (drift) {
    // only the topic that actually changed needs a re-publish; two small
    // packets beat one combined topic that both entities have to parse
    bool lightChanged =
        !snapValid || s.on != lastSnap.on || s.bri != lastSnap.bri ||
        s.anim != lastSnap.anim;
    bool addonChanged = !snapValid || s.addon != lastSnap.addon;
    lastSnap = s;
    snapValid = true;
    lastBeatMs = now;
    if (lightChanged) publishState();
    if (addonChanged) publishAddonState();
  } else if (now - lastBeatMs >= 30000UL) {
    lastBeatMs = now;
    publishState();
    publishAddonState();
  }
}

const char *mqttStateStr() {
  if (!settings.mqttEnable) return "off";
  return client.connected() ? "online" : "offline";
}
