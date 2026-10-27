#pragma once
#include <Arduino.h>

// Панели → «Включение по расписанию»: fires Settings::sched rules against
// the synced NTP clock. Call schedulerLoop() from the main loop; without a
// synced clock (< Nov 2023) it does nothing.
// NOTE: don't name this file sched.h — it collides with the newlib
// <sched.h> (extern "C") from the toolchain and links a C symbol.
void schedulerLoop();
