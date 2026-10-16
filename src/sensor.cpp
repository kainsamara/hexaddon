#include "sensor.h"

#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Wire.h>

#include "settings.h"

static Adafruit_BME280 bme;
static Adafruit_BMP280 bmp;
static bool useBme = false;
static bool present = false;
static uint8_t addr = 0;
static uint32_t lastTry = 0;
static uint32_t lastRead = 0;
static float t = NAN, h = NAN, p = NAN;
static const char *chip = "";

// chip id register 0xD0: 0x60 = BME280, 0x58 = BMP280
static uint8_t chipId(uint8_t a) {
  Wire.beginTransmission(a);
  Wire.write(0xD0);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(a, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

static bool tryBegin(uint8_t a) {
  uint8_t id = chipId(a);
  if (id == 0x60) {
    if (!bme.begin(a, &Wire)) return false;
    useBme = true;
    chip = "BME280";
  } else if (id == 0x58) {
    if (!bmp.begin(a)) return false;  // TwoWire passed via ctor
    useBme = false;
    chip = "BMP280";
  } else {
    return false;
  }
  addr = a;
  return true;
}

void sensorInit() {
  // Wire is already up (displayBegin); short timeout set there applies here too
  lastTry = 0;
  sensorLoop();
}

static void markAbsent(const char *why) {
  Serial.printf("[sensor] lost (%s), re-probe\n", why);
  present = false;
  t = h = p = NAN;
  chip = "";
  addr = 0;
  // wait a full probe interval — lastTry=0 would re-probe every loop
  // and flood a half-plugged bus (same class of hang as display hotplug)
  lastTry = millis() ? millis() : 1;
}

void sensorLoop() {
  uint32_t now = millis();

  if (!present) {
    if (lastTry != 0 && now - lastTry < 3000) return;
    lastTry = now ? now : 1;
    if (tryBegin(0x76) || tryBegin(0x77)) {
      present = true;
      lastRead = 0;
      Serial.printf("[sensor] %s found at 0x%02X\n", chip, addr);
    }
    return;
  }

  if (now - lastRead < 2000) return;
  lastRead = now;

  /* device gone? the chip-id register must still answer with the same id */
  uint8_t id = chipId(addr);
  if (id != (useBme ? 0x60 : 0x58)) {
    markAbsent(id ? "wrong chip id" : "no ack");
    return;
  }

  float rawT, rawP, rawH = NAN;
  if (useBme) {
    rawT = bme.readTemperature();
    rawH = bme.readHumidity();
    rawP = bme.readPressure() / 100.0f;  // hPa
  } else {
    rawT = bmp.readTemperature();
    rawP = bmp.readPressure() / 100.0f;
  }

  if (isnan(rawT) || isnan(rawP) || (useBme && isnan(rawH))) {
    markAbsent("read failed");
    return;
  }

  /* plausibility on raw values: BME/BMP280 ranges, catches garbage on a broken bus */
  if (rawT < -45.0f || rawT > 90.0f ||
      rawP < 250.0f || rawP > 1250.0f ||
      (useBme && (rawH < -1.0f || rawH > 101.0f))) {
    markAbsent("bad data");
    return;
  }

  t = rawT + settings.tempOffset;
  h = rawH;
  p = rawP * 0.750062f;  // hPa → mmHg
}

bool sensorPresent() { return present; }
uint8_t sensorAddr() { return addr; }
const char *sensorChip() { return present ? chip : ""; }
float sensorTemp() { return t; }
float sensorHum() { return h; }
float sensorPres() { return p; }
