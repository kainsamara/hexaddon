#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "leds.h"
#include "anim.h"
#include "button.h"
#include "display.h"
#include "net.h"
#include "sensor.h"
#include "settings.h"
#include "web.h"

// Serial service commands (type in the monitor + Enter):
//   RESET  - wipe the whole NVS partition (settings, Wi-Fi, images, records)
//            and reboot into a factory-fresh portal
//   REBOOT - just restart
static void serialCmd() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (buf.length() < 12) buf += c;
      continue;
    }
    if (!buf.length()) continue;
    buf.trim();
    buf.toUpperCase();
    if (buf == "RESET") {
      Serial.println(F("[cmd] erasing NVS..."));
      nvs_flash_erase();  // every namespace: hexaddon, phy calibration, etc
      nvs_flash_init();
      Serial.println(F("[cmd] NVS erased, rebooting"));
      delay(300);
      ESP.restart();
    } else if (buf.startsWith("TXPWR")) {
      // usage: TXPWR 8.5   (valid: 19.5 19 18.5 17 15 13 11 8.5 7 5 2 -1)
      // antenna/SWR diagnostics: if the wifi connects at a LOWER power but
      // dies at 19.5 dBm, the PA is fine and the antenna is mistuned
      float p = buf.substring(6).toFloat();
      if (p > 0 || buf.substring(6) == "-1") {
        netSetTxPower(p);
        Serial.printf("[cmd] tx power set, reconnecting\n");
        WiFi.disconnect();
        WiFi.reconnect();
      } else {
        Serial.println(F("[cmd] usage: TXPWR 19.5|19|18.5|17|15|13|11|8.5|7|5|2|-1"));
      }
    } else if (buf == "REBOOT") {
      Serial.println(F("[cmd] rebooting"));
      delay(300);
      ESP.restart();
    } else {
      Serial.printf("[cmd] unknown: %s (RESET | REBOOT)\n", buf.c_str());
    }
    buf = "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  // core libraries spam harmless-but-scary ERROR lines (WebServer
  // "handler not found" for LAN scanners, Wire/NVS probing noise).
  // Our own [boot]/[net]/[disp] logs use Serial.printf and stay.
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.printf("\n=== HEXADDON v%s ===\n", FW_VERSION);
  Serial.printf("[boot] reset_reason=%d\n", (int)esp_reset_reason());

  settings.load();
  settings.applyPins();
  Serial.println("[boot] settings ok");

  if (!LittleFS.begin(true)) {
    Serial.println("[fs] LittleFS mount failed");
  } else {
    Serial.println("[boot] fs ok");
  }

  ledsInit();
  Serial.println("[boot] leds ok");
  displayBegin();
  Serial.println("[boot] display ok");
  animBegin();
  Serial.println("[boot] anim ok");
  sensorInit();
  netBegin();
  webBegin();
  buttonBegin();
  Serial.println("[boot] ready");

  Serial.printf("[boot] panels=%d leds=%d anim=%d wifi=%s\n", settings.panels,
                settings.ledCount(), settings.anim,
                settings.wifiConfigured ? settings.wifiSsid.c_str() : "(portal)");
}

void loop() {
  serialCmd();
  netLoop();
  webLoop();
  buttonLoop();
  ledsLoop();
  sensorLoop();
  animLoop();
  displayLoop();
  // give idle/WiFi a slice even if something above ran long
  delay(0);
}
