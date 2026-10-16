#pragma once

#include <Arduino.h>

void sensorInit();
void sensorLoop();       // hotplug probe every 3s + read every 2s
bool sensorPresent();
uint8_t sensorAddr();    // 0x76 / 0x77
const char *sensorChip(); // "BME280" / "BMP280" / ""
float sensorTemp();      // C, NAN when absent
float sensorHum();       // %, NAN for BMP280 (no humidity)
float sensorPres();      // mmHg (converted from hPa)
