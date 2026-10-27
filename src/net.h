#pragma once

#include <Arduino.h>

void netBegin();
void netLoop();
bool netIsPortal();
void netPortalTrySta();  // portal is up: start STA alongside it (no reboot)
void netSetTxPower(float dbm);  // 0 = max (19.5 dBm); applied live
uint8_t netLastDisconnectReason();   // wifi_err_reason_t of the last STA drop, 0 = none
const char *netLastDisconnectName(); // "" or e.g. "4WAY_HANDSHAKE_TIMEOUT"
