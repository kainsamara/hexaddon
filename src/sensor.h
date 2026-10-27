#pragma once

#include <Arduino.h>

// 24 h ring buffer, one point per HIST_STEP_MIN minutes (~864 bytes in RAM)
#define HIST_N        288
#define HIST_STEP_MIN 5

void sensorInit();
void sensorLoop();       // hotplug probe every 3s + read every 2s
bool sensorPresent();
uint8_t sensorAddr();    // 0x76 / 0x77
const char *sensorChip(); // "BME280" / "BMP280" / ""
float sensorTemp();      // C, NAN when absent
float sensorHum();       // %, NAN for BMP280 (no humidity)
float sensorPres();      // mmHg (converted from hPa)

// last 24 h, oldest first: t[i] = temp ×10 °C, h[i] = humidity % or -1,
// ts[i] = unix time of the sample (0 = clock was not synced yet) — the
// chart uses it to auto-scale the time axis and show real gaps.
// Gaps (sensor absent) are simply skipped.
int sensorHistory(int16_t *t, int16_t *h, uint32_t *ts, int maxN);
