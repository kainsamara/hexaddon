#pragma once

#include <Arduino.h>

// Home Assistant / Alice integration. Pure MQTT with HA auto-discovery:
// the broker settings live in Settings (mqttEnable/Host/Port/User/Pass),
// the topic base is settings.hostname. Everything runs from mqttLoop() in
// the main Arduino loop — no cross-task races by construction.
void mqttBegin();
void mqttLoop();

// Settings changed over HTTP: drop the current session and reconnect with
// the new broker config on the next loop pass (no reboot).
void mqttRestart();

// "online" | "offline" | "off" (disabled) — for /api/status and the UI card
const char *mqttStateStr();
