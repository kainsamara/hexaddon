#pragma once

#include <Arduino.h>

void ledsInit();
void ledsApply();      // re-apply brightness / power limit after config change
void ledsClearFrame(); // drop persistence when led count / effect changes
void ledsLoop();

const char *ledsAnimName(uint8_t idx);
const char *ledsAnimNameEn(uint8_t idx);
