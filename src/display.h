#pragma once

#include <Arduino.h>

#define IMG_SIZE 1024  // 128x64 XBM
#define DISP_NUM 2
#define DISP_ADDR0 0x3C
#define DISP_ADDR1 0x3D

void displayBegin();
void displayLoop();
void displayToast(const String &msg);
void displayLoadImage(uint8_t addr);   // reload image of one display from NVS
void displayReinit();                  // recreate drivers after settings.driver change
void displayApplyFlip();               // apply 180 rotation from settings.flip[]
void displayScan();                    // probe I2C now (hotplug0x3C/0x3D into list)
bool displayHasImage(uint8_t addr);
bool displayPresent(uint8_t addr);
int displayPresentList(uint8_t *out, int maxN);  // filled with present 7-bit addresses
bool displaySaveImage(uint8_t addr, const uint8_t *data, size_t len);  // store to NVS
bool displayClearImage(uint8_t addr);  // drop image (factory reset path)
bool displayCopyImage(uint8_t addr, uint8_t *out);  // copy raw XBM (IMG_SIZE)
