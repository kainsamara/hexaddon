#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "settings.h"

void animBegin();
void animLoop();
void animRender(Adafruit_GFX &gfx, int dispIdx);
void animReset(int dispIdx);