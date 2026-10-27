#include "scheduler.h"

#include <time.h>

#include "display.h"
#include "settings.h"

// A rule fires once when the local minute matches (days mask + hh:mm).
// "on for N minutes" arms a one-shot RAM timer (lost on reboot — fine).

struct SchedTimer {
  bool active = false;
  uint32_t offAt = 0;   // millis() deadline
  uint8_t target = 0;   // same target as the rule that armed it
  bool turnOn = false;  // what to do when it fires (the opposite action)
};
static SchedTimer timers[MAX_SCHED_RULES];

static void applyAction(uint8_t target, bool on, const char *why) {
  if (target == 1) {
    if (settings.addonPower == on) return;
    settings.addonPower = on;
    settings.applyPins();
  } else {
    if (settings.animOn == on) return;
    settings.animOn = on;
  }
  settings.save();
  Serial.printf("[sched] %s: %s (%s)\n",
                target ? "addon power" : "backlight", on ? "on" : "off", why);
  displayToast(String(on ? "ON: " : "OFF: ") +
               (target ? "Power" : "Light"));
}

void schedulerLoop() {
  time_t now = time(nullptr);
  if (now < 1700000000L) return;  // clock not synced yet
  struct tm tv;
  localtime_r(&now, &tv);

  // one-shot duration timers (wrap-safe signed compare)
  uint32_t ms = millis();
  for (int i = 0; i < MAX_SCHED_RULES; i++) {
    if (timers[i].active && (int32_t)(ms - timers[i].offAt) >= 0) {
      timers[i].active = false;
      applyAction(timers[i].target, timers[i].turnOn, "timer");
    }
  }

  // fire each rule once per matching minute; tm_wday: 0=Sun..6=Sat
  static int16_t lastYday = -1;
  static int16_t lastMin = -1;
  if (tv.tm_yday == lastYday && tv.tm_hour * 60 + tv.tm_min == lastMin)
    return;
  lastYday = tv.tm_yday;
  lastMin = tv.tm_hour * 60 + tv.tm_min;

  // bit0=Mon..bit6=Sun  ->  tm_wday 0=Sun
  uint8_t dayBit = (uint8_t)(1 << ((tv.tm_wday + 6) % 7));

  for (int i = 0; i < MAX_SCHED_RULES; i++) {
    const SchedRule &r = settings.sched[i];
    if (!r.on) continue;
    if (!(r.days & dayBit)) continue;
    if (r.hour != tv.tm_hour || r.minute != tv.tm_min) continue;

    applyAction(r.target, r.enable, "schedule");

    // "for N minutes": arm a timer with the OPPOSITE action — works both
    // for "on for N" and "off for N" (and a re-run cannot revive a stale
    // timer of a plain rule)
    timers[i].active = false;
    if (r.durationMin > 0) {
      timers[i].offAt = millis() + (uint32_t)r.durationMin * 60000UL;
      timers[i].target = r.target;
      timers[i].turnOn = !r.enable;
      timers[i].active = true;
    }
  }
}
