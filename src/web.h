#pragma once

#include <Arduino.h>

void webBegin();
void webLoop();
bool webScanRecent();  // a /api/networks scan started recently — the radio
                       // may still be off-channel (the core's SCANNING bit
                       // lies after its own short timeout)
