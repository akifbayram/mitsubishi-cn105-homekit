#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_system.h>
#include "esp_utils.h"
#include "event_ring.h"

// Persistent device event log + lifetime health counters, NVS-backed
// (namespace "ac-events"). Survives reboots and power cycles — unlike the
// RTC-RAM crash counter in main.cpp — so support can answer "has this unit
// been rebooting or losing the heat pump overnight?" from the web UI.
//
// Events are RARE by design (boots, faults, resets — not per-poll or
// per-disconnect chatter): every append rewrites the ~390 B blob, which is
// fine for NVS wear only as long as that stays true. Session-only counters
// (WiFi drops) belong in their owning module, not here.
//
// Thread-safe: append/read may be called from any task after eventlog_init().

enum EventType : uint8_t {
    EV_BOOT = 1,            // code = esp_reset_reason_t
    EV_CRASH,               // code = esp_reset_reason_t (panic/WDT reset)
    EV_SAFE_MODE,           // crash-loop threshold hit; services skipped
    EV_OTA_INSTALLED,       // firmware accepted, about to reboot into it
    EV_CN105_ERROR,         // code = heat pump error code
    EV_CN105_LOST,          // heat pump serial link lost
    EV_CN105_RESTORED,      // heat pump serial link back
    EV_RECOVERY_AP,         // WiFi recovery access point raised
    EV_HK_RESET,            // HomeKit pairings reset via web UI
    EV_WIFI_CREDS_CHANGED,  // WiFi credentials changed via web UI/AP portal
                            // code 0 = new credentials applied, 1 = erased
};

// Load state from NVS and record the boot (plus EV_CRASH/EV_SAFE_MODE when
// applicable). Call once from app_main after NVS + logging are up.
void eventlog_init(esp_reset_reason_t reason, bool wasCrash, bool safeMode);

void eventlog_append(EventType type, uint8_t code = 0);

uint32_t eventlog_boot_count();                  // lifetime boots
uint32_t eventlog_crash_total();                 // lifetime crash-resets
uint32_t eventlog_session_count(EventType type); // appends of `type` this boot
bool     eventlog_safe_mode();                   // as passed to eventlog_init

// Append `"bootCount":..,"crashTotal":..,"events":[{...},..]` (no outer
// braces; newest event first) to `out`. Returns bytes written (0 on overflow).
int eventlog_json(char *out, size_t cap);

const char *eventlog_type_str(uint8_t type);

// Snake_case alias for esp_utils.h's resetReasonStr(), kept for main.cpp's
// boot banner. ONE table, not two — a reset reason added to the enum must
// never make the About card and the state push disagree. New callers should
// use resetReasonStr() directly. `static` matches the linkage of the function
// it forwards to: a plain `inline` here has external linkage, so each
// translation unit's definition would name a different resetReasonStr — ODR
// violation, no diagnostic required.
static inline const char *reset_reason_str(esp_reset_reason_t r) { return resetReasonStr(r); }
