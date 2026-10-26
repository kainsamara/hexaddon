#pragma once

#include <Arduino.h>

#define FW_VERSION "0.9.4"

// Wi-Fi TX power, dBm (0 = max 11.0). Serial command "TXPWR x" overrides
// it at runtime for experiments. Lower it when the antenna is mistuned:
// a strong PA into a bad SWR deafens the receiver's own handshake.
#define WIFI_TX_POWER_DBM 11.0f

// ---------------- Pins ----------------
#define PIN_LED   3   // WS2812 data
#define PIN_SDA   4   // display I2C
#define PIN_SCL   5   // display I2C
#define PIN_PWR   7   // addon power enable (HIGH = on)
#define PIN_BTN   9   // button to GND (internal pullup); GPIO9 = BOOT strap: held at reset -> bootloader mode
#define PIN_IO0   0   // free pin -> addon
#define PIN_IO1   1   // free pin -> addon

// ---------------- Limits ----------------
#define MAX_PANELS  32
#define PANEL_LEDS  5
#define MAX_LEDS    (MAX_PANELS * PANEL_LEDS)
#define NUM_ANIMS   16

enum class Scope : uint8_t { Chain = 0, Panel = 1 };
enum class DispMode : uint8_t { Info = 0, Image = 1, Off = 2, Sensors = 3, Anim = 4, Clock = 5 };
enum class AnimPreset : uint8_t { Offzone2024 = 0, Offzone2026 = 1, Addon2022 = 2 };
enum class GpioMode : uint8_t { Input = 0, PullUp = 1, Output = 2 };

// configurable button gestures: btnAct[0]=single click, [1]=double click,
// [2]=hold 2s, [3]=hold 8s. GPIO1 = PIN_IO0, GPIO2 = PIN_IO1.
enum BtnAction : uint8_t {
  BTN_LIGHT = 0,    // toggle backlight
  BTN_POWER = 1,    // toggle addon power
  BTN_DISPLAY = 2,  // cycle display mode
  BTN_GPIO1 = 3,    // toggle GPIO1 (PIN_IO0)
  BTN_GPIO2 = 4,    // toggle GPIO2 (PIN_IO1)
  BTN_REBOOT = 5,
  BTN_WIFI = 6,     // forget wifi + restart
  BTN_ACTIONS = 7
};
#define BTN_SLOTS 4

struct GpioCfg {
  GpioMode mode = GpioMode::Input;
  uint8_t value = 0;
};

struct Settings {
  // LEDs
  uint8_t panels = 1;          // 1..MAX_PANELS
  bool reverse = false;        // reverse chain order
  uint8_t brightness = 120;    // 0..255
  uint8_t speed = 60;          // 0..100
  uint8_t anim = 0;            // 0..NUM_ANIMS-1
  bool animOn = true;
  Scope scope = Scope::Panel;
  uint8_t colR = 255, colG = 128, colB = 0;
  bool colorRandom = false;    // effects pick random colors
  String order = "GRB";        // wire color order
  uint16_t powerMa = 500;     // power limit, 0 = unlimited

  // GPIO / addon
  bool addonPower = true;
  GpioCfg gpio0;
  GpioCfg gpio1;

  // Display (per address: [0]=0x3C, [1]=0x3D)
  DispMode displayMode[2] = {DispMode::Info, DispMode::Info};
  String driver[2] = {"ssd1306", "ssd1306"};  // ssd1306 | sh1106
  // ANIM presets per display (index 0 = 0x3C, 1 = 0x3D)
  AnimPreset animPreset[2] = {AnimPreset::Offzone2024, AnimPreset::Offzone2024};
  bool flip[2] = {false, false};  // rotate 180: [0]=0x3C, [1]=0x3D

  // Button gestures: single / double / hold 2s / hold 8s
  uint8_t btnAct[BTN_SLOTS] = {BTN_LIGHT, BTN_POWER, BTN_REBOOT, BTN_WIFI};

  // Sensor temperature correction, °C (-15..15, step 0.5)
  float tempOffset = 0.0f;

  // Timezone: POSIX TZ string (e.g. "MSK-3"), validated against TZ_LIST
  // and pushed into libc via setenv("TZ")/tzset() in sanitize()
  String tz = "MSK-3";

  // Web UI theme: "dark" (default) | "light" — stored for the page only
  String theme = "dark";

  // Network
  String hostname = "hexaddon";
  String wifiSsid;
  String wifiPass;
  bool wifiConfigured = false;

  int ledCount() const { return panels * PANEL_LEDS; }

  void load();
  void save();
  void setWifi(const String &ssid, const String &pass);
  void clearWifi();
  void factoryReset();
  void applyPins();
  void sanitize();

  String colorHex() const;
  void setColorHex(const String &hex);
};

extern Settings settings;

const char *gpioModeStr(GpioMode m);
GpioMode gpioModeFromStr(const String &s);
const char *dispModeStr(DispMode m);
DispMode dispModeFromStr(const String &s);
const char *btnActionStr(uint8_t a);
uint8_t btnActionFromStr(const String &s, uint8_t def);
const char *animPresetStr(AnimPreset p);
AnimPreset animPresetFromStr(const String &s);