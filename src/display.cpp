#include "anim.h"
#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

#include "leds.h"
#include "net.h"
#include "sensor.h"
#include "settings.h"

struct Disp {
  uint8_t addr = 0;  // 7-bit I2C address
  Adafruit_SSD1306 *ssd = nullptr;
  Adafruit_SH1106G *sh = nullptr;
  Adafruit_GFX *g = nullptr;
  bool present = false;
  bool needInit = false;  // hotplug seen; begin next loop (one per pass)
  uint8_t hits = 0;       // consecutive successful probes (debounce)
  uint8_t miss = 0;       // consecutive failed probes (debounce)
  bool hasImg = false;
  bool poweredOff = false;
  uint32_t lastRender = 0;  // per-display redraw timer
  uint8_t img[IMG_SIZE] = {};  // raw LSB-first XBM (as stored on FS)
  uint8_t bmp[IMG_SIZE] = {};  // MSB-first copy for Adafruit drawBitmap
};

static Disp disps[DISP_NUM];
static char toastMsg[48] = "";
static uint32_t toastUntil = 0;
static uint32_t lastScan = 0;
static bool dirty = true;
static bool wasToast = false;

static String imgKey(uint8_t addr) {  // NVS key: "img_3c"
  return "img_" + String(addr, HEX);
}

static String legacyPath(uint8_t addr) {  // old firmware stored files here
  return "/img_" + String(addr, HEX) + ".xbm";
}

static Disp *findDisp(uint8_t addr) {
  for (int i = 0; i < DISP_NUM; i++)
    if (disps[i].addr == addr) return &disps[i];
  return nullptr;
}

static void flush(Disp &d) {
  if (d.ssd) d.ssd->display();
  else if (d.sh) d.sh->display();
}

// ---------------- native GFX text (classic 6x8 ASCII font) ----------------

static void drawText(Adafruit_GFX &d, int x, int yTop, const char *s,
                     uint16_t color) {
  d.setTextSize(1);
  d.setCursor(x, yTop);
  d.setTextColor(color);
  d.print(s);
}

// cut to N chars, append ".." when truncated
static String fitStr(const String &s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}

// ---------------- drivers ----------------

static void destroyDriver(Disp &d) {
  if (d.ssd) delete d.ssd;
  if (d.sh) delete d.sh;
  d.ssd = nullptr;
  d.sh = nullptr;
  d.g = nullptr;
}

static void createDriver(Disp &d) {
  destroyDriver(d);
  int idx = (int)(&d - disps);
  Serial.printf("[disp] new driver 0x%02X\n", d.addr);
  if (settings.driver[idx] == "sh1106") {
    d.sh = new (std::nothrow) Adafruit_SH1106G(128, 64, &Wire, -1);
    d.g = d.sh;
  } else {
    d.ssd = new (std::nothrow) Adafruit_SSD1306(128, 64, &Wire, -1);
    d.g = d.ssd;
  }
  if (!d.g) Serial.printf("[disp] driver alloc failed 0x%02X\n", d.addr);
}

// called when the panel answers on I2C; returns success
static bool initDisp(Disp &d) {
  if (!d.g) return false;  // driver alloc failed in createDriver()
  Serial.printf("[disp] begin 0x%02X\n", d.addr);
  bool ok;
  if (d.ssd) ok = d.ssd->begin(SSD1306_SWITCHCAPVCC, d.addr);
  else ok = d.sh->begin(d.addr, false);
  Serial.printf("[disp] began 0x%02X %s\n", d.addr, ok ? "ok" : "FAIL");
  if (!ok) return false;
  int idx = (int)(&d - disps);
  d.g->setRotation(settings.flip[idx] ? 2 : 0);
  d.g->setFont(nullptr);
  d.g->setTextSize(1);
  d.g->fillScreen(0);
  flush(d);
  d.poweredOff = false;
  return true;
}

static bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Only mark devices present/absent here — never call the heavy
// driver begin() from the scan path. Hotplug during a live bus can
// produce a single false ACK; require 2 consecutive hits. Actual
// initDisp() runs at most once per displayLoop so the web server
// keeps getting CPU while the I2C bus settles.
static void scan() {
  for (int i = 0; i < DISP_NUM; i++) {
    Disp &d = disps[i];
    bool p = probe(d.addr);

    if (p) {
      d.miss = 0;
      if (d.hits < 2) d.hits++;
      if (d.hits >= 2 && !d.present && !d.needInit) {
        d.needInit = true;
        Serial.printf("[disp] 0x%02X detected, init queued\n", d.addr);
      }
    } else {
      // one glitchy NACK must not tear the display down: need 2 in a row
      if (d.miss < 2) d.miss++;
      if (d.miss < 2) continue;
      d.hits = 0;
      if (d.present || d.needInit) {
        Serial.printf("[disp] 0x%02X lost\n", d.addr);
        d.present = false;
        d.needInit = false;
        dirty = true;
      }
    }
  }
}

// init at most one queued display per call
static void servicePendingInit() {
  for (int i = 0; i < DISP_NUM; i++) {
    Disp &d = disps[i];
    if (!d.needInit || d.present) continue;
    d.needInit = false;
    // device may have been yanked between scan and here
    if (!probe(d.addr)) {
      d.hits = 0;
      continue;
    }
    bool ok = initDisp(d);
    Serial.printf("[disp] 0x%02X %s\n", d.addr, ok ? "connected" : "init fail");
    d.present = ok;
    if (ok) dirty = true;
    return;  // one heavy I2C init per loop pass
  }
}

void displayBegin() {
  Serial.println("[disp] begin");
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(20);  // default 50ms × many cmds can freeze the web UI on a flaky hotplug bus
  disps[0].addr = DISP_ADDR0;
  disps[1].addr = DISP_ADDR1;
  for (int i = 0; i < DISP_NUM; i++) {
    Serial.printf("[disp] init 0x%02X\n", disps[i].addr);
    createDriver(disps[i]);
    displayLoadImage(disps[i].addr);
    Serial.printf("[disp] image 0x%02X %d\n", disps[i].addr, (int)disps[i].hasImg);
  }
  // two quick probes so units already plugged at boot are debounced in
  scan();
  delay(5);
  scan();
  servicePendingInit();
  for (int i = 0; i < DISP_NUM; i++) {
    Serial.printf("[disp] 0x%02X %s image=%d\n", disps[i].addr,
                  disps[i].present ? "present" : "absent",
                  (int)disps[i].hasImg);
  }
  lastScan = millis();
  dirty = true;
}

void displayScan() {
  scan();
  dirty = true;
}

void displayApplyFlip() {
  for (int i = 0; i < DISP_NUM; i++) {
    if (disps[i].present && disps[i].g)
      disps[i].g->setRotation(settings.flip[i] ? 2 : 0);
  }
  dirty = true;
}

void displayReinit() {
  for (int i = 0; i < DISP_NUM; i++) {
    destroyDriver(disps[i]);
    createDriver(disps[i]);
    disps[i].present = false;  // force re-begin on next probe
    disps[i].needInit = false;
    disps[i].hits = 0;
  }
  scan();
  delay(5);
  scan();
  servicePendingInit();
  dirty = true;
}

void displayLoadImage(uint8_t addr) {
  Disp *d = findDisp(addr);
  if (!d) return;
  d->hasImg = false;
  String key = imgKey(addr);
  Preferences p;
  p.begin("hexaddon", false);
  size_t n = 0;
  // isKey() first: a plain getBytes() on a missing key makes the core
  // log an ERROR line ("nvs_get_blob len fail") on every boot
  if (p.isKey(key.c_str()))
    n = p.getBytes(key.c_str(), d->img, IMG_SIZE);
  if (n != IMG_SIZE) {
    // image still in LittleFS from an old firmware -> migrate to NVS once.
    // Checked once per boot: exists() on a missing file logs a vfs error,
    // and the legacy file can't appear later anyway.
    static bool legacyDone[DISP_NUM] = {false, false};
    int di = (addr == DISP_ADDR0) ? 0 : 1;
    if (!legacyDone[di]) {
      legacyDone[di] = true;
      String path = legacyPath(addr);
      if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
          n = f.read(d->img, IMG_SIZE);
          f.close();
          if (n == IMG_SIZE &&
              p.putBytes(key.c_str(), d->img, IMG_SIZE) == IMG_SIZE) {
            LittleFS.remove(path);
            Serial.printf("[disp] image 0x%02X moved to NVS\n", addr);
          }
        }
      }
    }
  }
  p.end();
  d->hasImg = (n == IMG_SIZE);
  if (d->hasImg) {
    // XBM is LSB-first, Adafruit drawBitmap wants MSB-first: reverse bits
    for (int i = 0; i < IMG_SIZE; i++) {
      uint8_t b = d->img[i];
      b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
      b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
      b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
      d->bmp[i] = b;
    }
  }
  dirty = true;
}

bool displaySaveImage(uint8_t addr, const uint8_t *data, size_t len) {
  if (!findDisp(addr) || !data || len != IMG_SIZE) return false;
  Preferences p;
  p.begin("hexaddon", false);
  size_t w = p.putBytes(imgKey(addr).c_str(), data, IMG_SIZE);
  p.end();
  if (w != IMG_SIZE) return false;
  displayLoadImage(addr);
  return true;
}

bool displayClearImage(uint8_t addr) {
  Disp *d = findDisp(addr);
  if (!d) return false;
  Preferences p;
  p.begin("hexaddon", false);
  p.remove(imgKey(addr).c_str());
  p.end();
  memset(d->img, 0, IMG_SIZE);
  memset(d->bmp, 0, IMG_SIZE);
  d->hasImg = false;
  dirty = true;
  return true;
}

bool displayCopyImage(uint8_t addr, uint8_t *out) {
  Disp *d = findDisp(addr);
  if (!d || !d->hasImg || !out) return false;
  memcpy(out, d->img, IMG_SIZE);
  return true;
}

bool displayHasImage(uint8_t addr) {
  Disp *d = findDisp(addr);
  return d && d->hasImg;
}

bool displayPresent(uint8_t addr) {
  Disp *d = findDisp(addr);
  return d && d->present;
}

int displayPresentList(uint8_t *out, int maxN) {
  int n = 0;
  for (int i = 0; i < DISP_NUM && n < maxN; i++) {
    if (disps[i].present) out[n++] = disps[i].addr;
  }
  return n;
}

void displayToast(const String &msg) {
  String s = msg;
  if (s.length() >= sizeof(toastMsg)) s = s.substring(0, sizeof(toastMsg) - 1);
  strncpy(toastMsg, s.c_str(), sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = 0;
  toastUntil = millis() + 2000;
  dirty = true;
}

// ---------------- content ----------------

static String gpioDesc(int n, const GpioCfg &g, bool live) {
  String s = "D" + String(n) + ":";
  switch (g.mode) {
    case GpioMode::Output: {
      s += "out";
      int v = live ? digitalRead(n == 0 ? PIN_IO0 : PIN_IO1) : g.value;
      s += String(v);
      break;
    }
    case GpioMode::PullUp: {
      s += "pu";
      int v = digitalRead(n == 0 ? PIN_IO0 : PIN_IO1);
      s += String(v);
      break;
    }
    default: {
      s += "in";
      int v = digitalRead(n == 0 ? PIN_IO0 : PIN_IO1);
      s += String(v);
      break;
    }
  }
  return s;
}

static void drawInfo(Adafruit_GFX &d) {
  String l[5];

  if (netIsPortal()) {
    // the AP itself failed to start: the web UI is unreachable — say so
    // on the screen instead of showing a network nobody can join
    bool apBad = netApFailed();
    l[0] = apBad ? "AP START FAILED" : "SETUP MODE";
    l[1] = "AP: " + fitStr(String(WiFi.softAPSSID()), 17);
    l[2] = apBad ? "Web UI unreachable!" : "Open network";
    l[3] = "IP: " + WiFi.softAPIP().toString();
    if (settings.wifiConfigured) {
      l[4] = "No conn: " + fitStr(settings.wifiSsid, 12);
    } else {
      l[4] = "Ready for config";
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    l[0] = "IP: " + WiFi.localIP().toString();
    l[1] = "WiFi: " + fitStr(WiFi.SSID(), 15);
    l[2] = String(ledsAnimNameEn(settings.anim)) + "  " +
            String(settings.panels) + " hex.";
    l[3] = String("Power: ") + (settings.addonPower ? "ON" : "OFF");
    l[4] = gpioDesc(0, settings.gpio0, true) + "  " +
           gpioDesc(1, settings.gpio1, true);
  } else {
    l[0] = "HEXADDON";
    l[1] = "No WiFi network";
    l[2] = String(ledsAnimNameEn(settings.anim));
    l[3] = String("Power: ") + (settings.addonPower ? "ON" : "OFF");
    l[4] = "IP: none";
  }

  int y = 4;
  for (int i = 0; i < 5; i++) {
    String t = fitStr(l[i], 21);
    drawText(d, 0, y, t.c_str(), 1);
    y += 11;
  }
}

static void drawNoImage(Adafruit_GFX &d) {
  const char *m = "NO PICTURE";
  drawText(d, (128 - 6 * (int)strlen(m)) / 2, 28, m, 1);
}

// NTP clock: big HH:MM with a blinking colon + date line
static void drawClock(Adafruit_GFX &d) {
  time_t now = time(nullptr);
  if (now < 1700000000L) {  // before Nov 2023 = not synced yet
    const char *m = "NO TIME";
    drawText(d, (128 - 6 * (int)strlen(m)) / 2, 22, m, 1);
    const char *s = "waiting for NTP";
    drawText(d, (128 - 6 * (int)strlen(s)) / 2, 38, s, 1);
    return;
  }
  struct tm tv;
  localtime_r(&now, &tv);
  char b[8];
  snprintf(b, sizeof(b), "%02d%c%02d", tv.tm_hour, (tv.tm_sec & 1) ? ':' : ' ',
           tv.tm_min);
  d.setTextSize(3);
  d.setTextColor(1);
  d.setCursor((128 - 5 * 18) / 2, 12);
  d.print(b);
  char dt[16];
  snprintf(dt, sizeof(dt), "%02d.%02d.%04d", tv.tm_mday, tv.tm_mon + 1,
           tv.tm_year + 1900);
  drawText(d, (128 - 6 * (int)strlen(dt)) / 2, 48, dt, 1);
}

static void drawSensors(Adafruit_GFX &d) {
  if (!sensorPresent()) {
    const char *l1 = "BME280/BMP280", *l2 = "not found", *l3 = "0x76 / 0x77";
    drawText(d, (128 - 6 * (int)strlen(l1)) / 2, 14, l1, 1);
    drawText(d, (128 - 6 * (int)strlen(l2)) / 2, 30, l2, 1);
    drawText(d, (128 - 6 * (int)strlen(l3)) / 2, 46, l3, 1);
    return;
  }
  const char *lab[3] = {"TEMP C", "HUM %", "P mmHg"};
  float val[3] = {sensorTemp(), sensorHum(), sensorPres()};
  int y = 2;
  for (int i = 0; i < 3; i++) {
    if (isnan(val[i])) {  // BMP280 has no humidity — leave the row black
      y += 20;
      continue;
    }
    drawText(d, 0, y + 6, lab[i], 1);
    char v[12];
    snprintf(v, sizeof(v), "%.1f", val[i]);
    d.setTextSize(2);
    d.setCursor(54, y);
    d.setTextColor(1);
    d.print(v);
    d.setTextSize(1);
    y += 20;
  }
}

static void render(Disp &d, bool toastActive) {
  if (!d.g) return;
  int idx = (int)(&d - disps);
  DispMode mode = settings.displayMode[idx];
  d.g->fillScreen(0);
  if (mode == DispMode::Image) {
    if (d.hasImg) d.g->drawBitmap(0, 0, d.bmp, 128, 64, 1);
    else drawNoImage(*d.g);
  } else if (mode == DispMode::Sensors) {
    drawSensors(*d.g);
  } else if (mode == DispMode::Anim) {
    animRender(*d.g, idx);
  } else if (mode == DispMode::Clock) {
    drawClock(*d.g);
  } else {
    drawInfo(*d.g);
  }	
  if (toastActive && toastMsg[0]) {
    d.g->fillRect(0, 52, 128, 12, 1);
    String t = fitStr(String(toastMsg), 20);
    drawText(*d.g, 2, 54, t.c_str(), 0);
  }
  flush(d);
}

static void offDisp(Disp &d) {
  d.g->fillScreen(0);
  flush(d);
  if (d.ssd) d.ssd->ssd1306_command(SSD1306_DISPLAYOFF);
  else if (d.sh) d.sh->oled_command(SH110X_DISPLAYOFF);
}

static void onDisp(Disp &d) {
  if (d.ssd) d.ssd->ssd1306_command(SSD1306_DISPLAYON);
  else if (d.sh) d.sh->oled_command(SH110X_DISPLAYON);
  dirty = true;
}

void displayLoop() {
  uint32_t now = millis();

  // Displays live on the addon power rail. With the rail off the bus is
  // dead-ish: a half-powered panel parasitizes through the pull-ups, ACKs
  // randomly and every Wire call burns its 20ms timeout — the loop freezes.
  // Skip ALL i2c work and drop the panels until the power returns.
  if (!settings.addonPower) {
    for (int i = 0; i < DISP_NUM; i++) {
      if (disps[i].present || disps[i].needInit) {
        disps[i].present = false;
        disps[i].needInit = false;
        disps[i].hits = 0;
        dirty = true;
      }
    }
    return;  // no scan, no init, no render
  }

  if (now - lastScan >= 2000) {
    lastScan = now;
    scan();
  }
  servicePendingInit();

  bool anyPresent = false;
  for (int i = 0; i < DISP_NUM; i++)
    if (disps[i].present) anyPresent = true;
  if (!anyPresent) return;

  bool toastActive = (int32_t)(now - toastUntil) < 0;  // wrap-safe
  if (wasToast && !toastActive) dirty = true;
  wasToast = toastActive;

  for (int i = 0; i < DISP_NUM; i++) {
    Disp &d = disps[i];
    if (!d.present) continue;
    if (settings.displayMode[i] == DispMode::Off) {
      if (!d.poweredOff) {
        offDisp(d);
        d.poweredOff = true;
      }
    } else if (d.poweredOff) {
      onDisp(d);
      d.poweredOff = false;
    }
  }

  // Per-display redraw pacing: ANIM displays need high fps, everything else
  // is static content (1s heartbeat / 250ms while a toast is shown).
  int animCount = 0;
  for (int i = 0; i < DISP_NUM; i++)
    if (disps[i].present && !disps[i].poweredOff &&
        settings.displayMode[i] == DispMode::Anim)
      animCount++;
  // one full-frame I2C flush is ~24ms @400kHz: two ANIM displays never fit
  // into a 40ms window and starve the LED loop, slow them to 80ms instead
  uint32_t animIv = animCount > 1 ? 80 : 40;

  bool allDirty = dirty;
  dirty = false;

  for (int i = 0; i < DISP_NUM; i++) {
    Disp &d = disps[i];
    if (!d.present || d.poweredOff) continue;
    bool animMode = (settings.displayMode[i] == DispMode::Anim);
    bool clockMode = (settings.displayMode[i] == DispMode::Clock);
    uint32_t interval =
        toastActive ? 250 : (animMode ? animIv : (clockMode ? 500 : 1000));
    if (!allDirty && now - d.lastRender < interval) continue;
    d.lastRender = now;
    render(d, toastActive);
  }
}
