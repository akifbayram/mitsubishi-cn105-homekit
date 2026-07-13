#pragma once

// SNTP wall-clock sync. Without it every timestamp on the device is
// boot-relative; with it the event log and diagnostics can say WHEN
// something happened. Purely best-effort: nothing depends on sync succeeding.

#include <cstdint>

// Start SNTP (idempotent; call once WiFi is connected — non-blocking).
void time_sync_start();

// Current unix time, or 0 while the clock has never been set. The single
// source of truth for "is this a real wall-clock time?" — everything that
// stamps or reports epochs goes through here.
uint32_t time_sync_epoch();

// True once the system clock holds a plausible wall-clock time.
bool time_sync_valid();
