#pragma once

#include <Arduino.h>

void netBegin();
void netLoop();
bool netIsPortal();
bool netApFailed();      // portal is up but the AP itself failed to start
void netPortalTrySta();  // portal is up: start STA alongside it (no reboot)
void netSetTxPower(float dbm, bool manual = false);  // 0 = max (19.5 dBm); manual (serial TXPWR) overrides the board ceiling until reboot
uint8_t netLastDisconnectReason();   // wifi_err_reason_t of the last STA drop, 0 = none
const char *netLastDisconnectName(); // "" or e.g. "4WAY_HANDSHAKE_TIMEOUT"
bool netMdnsSelfOk();  // last mDNS liveness self-check (false = name dead, IP alive)
