#pragma once

#include <Arduino.h>

// history ring: the last HIST_N samples, one per settings.histStepMin
// minutes (1/2/5/10/15/30, default 5) — ~1 KB in RAM
#define HIST_N 100

void sensorInit();
void sensorLoop();       // hotplug probe every 3s + read every 2s
bool sensorPresent();
uint8_t sensorAddr();    // 0x76 / 0x77
const char *sensorChip(); // "BME280" / "BMP280" / ""
float sensorTemp();      // C, NAN when absent
float sensorHum();       // %, NAN for BMP280 (no humidity)
float sensorPres();      // mmHg (converted from hPa)

// oldest first: t[i] = temp ×10 °C, h[i] = humidity % or -1,
// p[i] = pressure mmHg ×10, ts[i] = unix time of the sample (0 = clock
// was not synced yet) — the chart uses it to auto-scale the time axis and
// show real gaps. Gaps (sensor absent) are simply skipped.
int sensorHistory(int16_t *t, int16_t *h, int16_t *p, uint32_t *ts, int maxN);

void sensorHistFlush();  // push the ring to NVS now (before a graceful reboot)
void sensorHistClear();  // factory reset: drop the RAM ring so it cannot be re-saved
