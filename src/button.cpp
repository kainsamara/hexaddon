#include "button.h"

#include "display.h"
#include "settings.h"
#include "sensor.h"

// configurable gestures (settings.btnAct[]):
//   single click / double click / hold 2s / hold 8s
// Hold actions fire on RELEASE, by the threshold reached: firing the 2s
// slot right at the 2s mark would (with the default "reboot" there) restart
// the board before the 8s action can ever run, and keep restarting in a
// loop while the button stays pressed.

#define BTN_DEBOUNCE_MS 25
#define BTN_HOLD2_MS    2000
#define BTN_HOLD8_MS    8000
#define BTN_DBL_MS      350  // max pause between the two clicks

static void displayCycle() {
  // same order as the UI list, sensors is skipped when no sensor is connected
  static const DispMode ORDER[6] = {DispMode::Info, DispMode::Image,
                                    DispMode::Anim, DispMode::Sensors,
                                    DispMode::Clock, DispMode::Off};
  int cur = 0;
  for (int i = 0; i < 6; i++)
    if (settings.displayMode[0] == ORDER[i]) cur = i;
  DispMode next;
  do {
    cur = (cur + 1) % 6;
    next = ORDER[cur];
  } while (next == DispMode::Sensors && !sensorPresent());
  settings.displayMode[0] = settings.displayMode[1] = next;
  settings.save();
  const char *nm = next == DispMode::Info      ? "Info"
                   : next == DispMode::Image   ? "Image"
                   : next == DispMode::Anim    ? "Anim"
                   : next == DispMode::Sensors ? "Sensors"
                   : next == DispMode::Clock   ? "Clock"
                                               : "Off";
  displayToast(String("Mode: ") + nm);
}

static void gpioToggle(int which) {
  GpioCfg &g = which ? settings.gpio1 : settings.gpio0;
  g.mode = GpioMode::Output;
  g.value = g.value ? 0 : 1;
  settings.applyPins();
  settings.save();
  displayToast(String(which ? "GPIO2: " : "GPIO1: ") + (g.value ? "ON" : "OFF"));
}

static void runAction(uint8_t a) {
  switch (a) {
    case BTN_POWER:
      settings.addonPower = !settings.addonPower;
      settings.applyPins();
      settings.save();
      displayToast(settings.addonPower ? "Power: ON" : "Power: OFF");
      break;

    case BTN_DISPLAY:
      displayCycle();
      break;

    case BTN_GPIO1:
      gpioToggle(0);
      break;

    case BTN_GPIO2:
      gpioToggle(1);
      break;

    case BTN_REBOOT:
      displayToast("Reboot...");
      delay(300);
      ESP.restart();
      break;

    case BTN_WIFI:
      displayToast("Wi-Fi reset...");
      settings.clearWifi();
      delay(400);
      ESP.restart();
      break;

    default:  // BTN_LIGHT
      settings.animOn = !settings.animOn;
      settings.save();
      displayToast(settings.animOn ? "Backlight: ON" : "Backlight: OFF");
      break;
  }
}

void buttonBegin() { pinMode(PIN_BTN, INPUT_PULLUP); }

void buttonLoop() {
  static bool down = false;
  static bool raw = false;
  static uint32_t rawSince = 0;
  static uint32_t downAt = 0;
  static uint32_t pendingClickAt = 0;  // 1st click waiting for a possible 2nd

  bool r = (digitalRead(PIN_BTN) == LOW);
  uint32_t now = millis();

  if (r != raw) {
    raw = r;
    rawSince = now;
    return;
  }
  if (now - rawSince < BTN_DEBOUNCE_MS) return;  // debounce settled
  bool pressed = raw;

  // no second click in time -> fire the single click action.
  // only while released: firing mid-press would run it twice (the
  // release path below would then re-arm it).
  if (pendingClickAt && !pressed && now - pendingClickAt >= BTN_DBL_MS) {
    pendingClickAt = 0;
    runAction(settings.btnAct[0]);
  }

  if (pressed && !down) {
    down = true;
    downAt = now;
    return;
  }

  if (!pressed && down) {
    down = false;
    uint32_t held = now - downAt;

    // hold gestures: which slot fires is decided by how long the button
    // was held IN TOTAL, when it's released (see the comment on top)
    if (held >= BTN_HOLD8_MS) {
      pendingClickAt = 0;  // this was a hold, not a click
      runAction(settings.btnAct[3]);
      return;
    }
    if (held >= BTN_HOLD2_MS) {
      pendingClickAt = 0;
      runAction(settings.btnAct[2]);
      return;
    }
    if (pendingClickAt) {
      pendingClickAt = 0;
      // quick 2nd press = double click; a slower one just completes
      // the first click (its action already fired via the timeout above
      // or fires here)
      if (held < BTN_DBL_MS) {
        runAction(settings.btnAct[1]);
        return;
      }
      runAction(settings.btnAct[0]);
      return;
    }
    pendingClickAt = now;  // wait a bit for a possible second click
  }
}
