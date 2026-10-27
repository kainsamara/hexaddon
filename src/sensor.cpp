#include "sensor.h"

#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>

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

// history ring (see sensor.h)
static int16_t hT[HIST_N];  // temp ×10 °C
static int16_t hH[HIST_N];  // humidity %, -1 = none (BMP280)
static int16_t hP[HIST_N];  // pressure mmHg ×10
static uint32_t hTs[HIST_N];  // unix time of the sample (0 = clock unsynced)
static int hHead = 0, hCount = 0;
static uint32_t hAt = 0;
static uint8_t hLastStep = 0xFF;  // step the current ring was filled with
static bool hDirty = false;       // samples appended since the last NVS save

// The WHOLE ring is snapshotted to NVS (blob key "hist") every 30 min and
// flushed on graceful reboots (sensorHistFlush), then restored on boot —
// the chart survives reboots losing at most the last 30 min (power cut).
// ~1 KB per write × 48/day spread over the NVS pages ≈ 11 years of flash.
#define HIST_KEEP_MS (30UL * 60UL * 1000UL)
static uint32_t hKeepAt = 0;

// blob = [u16 count][per point: t, h, p (i16 LE), ts (u32 LE)], oldest first
#define HIST_BLOB_MAX (2 + HIST_N * 10)
static uint8_t histBlob[HIST_BLOB_MAX];  // loop context only, ~1 KB off-stack

static uint32_t histWindowS() {
  return (uint32_t)HIST_N * settings.histStepMin * 60UL;
}

static void put16(uint8_t *b, int16_t v) {
  b[0] = (uint8_t)((uint16_t)v & 0xFF);
  b[1] = (uint8_t)((uint16_t)v >> 8);
}
static void put32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 24);
}
static int16_t get16(const uint8_t *b) {
  return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static uint32_t get32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static void histKeepSave() {
  if (!hCount || !hDirty) return;
  int start = (hHead - hCount + HIST_N) % HIST_N;
  histBlob[0] = (uint8_t)(hCount & 0xFF);
  histBlob[1] = (uint8_t)((uint32_t)hCount >> 8);
  int o = 2;
  for (int i = 0; i < hCount; i++) {
    int idx = (start + i) % HIST_N;
    put16(histBlob + o, hT[idx]);
    put16(histBlob + o + 2, hH[idx]);
    put16(histBlob + o + 4, hP[idx]);
    put32(histBlob + o + 6, hTs[idx]);
    o += 10;
  }
  Preferences p;
  p.begin("hexaddon", false);
  p.putBytes("hist", histBlob, o);
  p.end();
  hDirty = false;
}

// Restore the ring saved before the last reboot. The wall clock is never
// synced this early in boot, so staleness is judged RELATIVE to the newest
// restored point: keep its trailing histWindowS() tail (also trims the
// head when the step was enlarged across the reboot).
static void histKeepLoad() {
  Preferences p;
  p.begin("hexaddon", true);
  if (!p.isKey("hist")) {
    p.end();
    return;
  }
  size_t n = p.getBytes("hist", histBlob, sizeof(histBlob));
  p.end();
  if (n < 12) return;  // 8/10 = single-point pre-ring formats: dropped
  int cnt = (int)histBlob[0] | ((int)histBlob[1] << 8);
  if (cnt <= 0 || cnt > HIST_N || (size_t)(2 + cnt * 10) != n) return;

  // pass 1: validate every point, find the newest timestamp
  uint32_t newest = 0;
  for (int i = 0; i < cnt; i++) {
    const uint8_t *e = histBlob + 2 + i * 10;
    if (get16(e) < -600 || get16(e) > 1050 ||  // temp ×10 °C
        get16(e + 2) < -1 || get16(e + 2) > 100 ||  // humidity %
        get16(e + 4) < 3000 || get16(e + 4) > 13000)  // mmHg ×10
      return;  // corrupt blob: an empty chart beats a garbage one
    uint32_t ts = get32(e + 6);
    if (ts > newest) newest = ts;
  }

  // pass 2: copy the fresh tail (ts == 0 — an unsynced sample has no
  // place on the real time axis)
  uint32_t window = histWindowS();
  int k = 0;
  for (int i = 0; i < cnt; i++) {
    const uint8_t *e = histBlob + 2 + i * 10;
    uint32_t ts = get32(e + 6);
    if (!ts || newest - ts > window) continue;
    hT[k] = get16(e);
    hH[k] = get16(e + 2);
    hP[k] = get16(e + 4);
    hTs[k] = ts;
    k++;
  }
  hHead = k % HIST_N;  // k == HIST_N: ring full, head back at the oldest
  hCount = k;
}

// chip id register 0xD0: 0x60 = BME280, 0x58 = BMP280
static uint8_t chipId(uint8_t a) {
  Wire.beginTransmission(a);
  Wire.write(0xD0);
  // plain STOP (not a repeated start): a missing device NACKs here, so we
  // never reach requestFrom() — that call logs a scary Wire.cpp error
  // every 3s when no sensor is attached
  if (Wire.endTransmission() != 0) return 0;
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
  // ring bookkeeping FIRST: the boot-time step reset must not wipe what
  // histKeepLoad() restores two lines below (the reboot data-loss bug)
  if (hLastStep != settings.histStepMin) {
    hLastStep = settings.histStepMin;
    hHead = 0;
    hCount = 0;
    hAt = 0;
  }
  histKeepLoad();  // restore the ring that survived the last reboot
  hKeepAt = millis() ? millis() : 1;
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

  // a changed sample period invalidates the ring's spacing: start it over
  // (hLastStep is primed in sensorInit(), so this never fires at boot and
  // cannot wipe the restored ring; leaves hAt = 0, so the very first good
  // reading lands in the chart immediately, not after a whole interval)
  if (hLastStep != settings.histStepMin) {
    hLastStep = settings.histStepMin;
    hHead = 0;
    hCount = 0;
    hAt = 0;
  }

  // sensor sits on the addon rail too: never probe it with power off,
  // a dead-ish bus only burns Wire timeouts
  if (!settings.addonPower) {
    if (present) markAbsent("addon power off");
    return;
  }

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

  // one history point per configured interval, valid readings only
  // (hAt == 0 = the first sample after a reset: capture right away)
  if (!hAt || now - hAt >= (uint32_t)settings.histStepMin * 60000UL) {
    hAt = now;
    time_t wall = time(nullptr);
    hTs[hHead] = wall > 1700000000L ? (uint32_t)wall : 0;
    hT[hHead] = (int16_t)lroundf(t * 10.0f);
    hH[hHead] = isnan(h) ? -1 : (int16_t)lroundf(h);
    hP[hHead] = (int16_t)lroundf(p * 10.0f);
    hHead = (hHead + 1) % HIST_N;
    if (hCount < HIST_N) hCount++;
    hDirty = true;
    // every 30 min the whole ring goes to NVS (reboot survival)
    if (now - hKeepAt >= HIST_KEEP_MS) {
      hKeepAt = now;
      histKeepSave();
    }
  }
}

int sensorHistory(int16_t *t, int16_t *h, int16_t *p, uint32_t *ts, int maxN) {
  int n = hCount < maxN ? hCount : maxN;
  int start = (hHead - n + HIST_N) % HIST_N;
  for (int i = 0; i < n; i++) {
    int idx = (start + i) % HIST_N;
    t[i] = hT[idx];
    h[i] = hH[idx];
    p[i] = hP[idx];
    ts[i] = hTs[idx];
  }
  return n;
}

void sensorHistFlush() { histKeepSave(); }  // graceful reboot: keep the chart

void sensorHistClear() {
  // factory reset: forget the RAM ring too, or the pre-reboot flush would
  // re-save it into the just-wiped NVS
  hHead = 0;
  hCount = 0;
  hAt = 0;
  hDirty = false;
}

bool sensorPresent() { return present; }
uint8_t sensorAddr() { return addr; }
const char *sensorChip() { return present ? chip : ""; }
float sensorTemp() { return t; }
float sensorHum() { return h; }
float sensorPres() { return p; }
