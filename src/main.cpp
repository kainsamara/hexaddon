#include <Arduino.h>
#include <LittleFS.h>

#include "leds.h"
#include "anim.h"
#include "button.h"
#include "display.h"
#include "net.h"
#include "sensor.h"
#include "settings.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  delay(200);
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
