#pragma once
#include <stdint.h>
#include <stddef.h>
#include <esp_timer.h>
#include <esp_system.h>

static inline uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Human-readable esp_reset_reason() — boot banner (main.cpp) and the web
// state push share this so a fleet unit's reboots are classifiable remotely.
static inline const char *resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_SW:        return "SW_RESTART";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        default:                return "OTHER";
    }
}

// Consecutive-crash counter from the RTC-noinit crash-loop detector
// (defined in main.cpp; 0 after any clean boot).
uint32_t appCrashCount(void);

// FNV-1a 32-bit content hash (setup-code derivation, CAPS fingerprint).
// Deterministic across boots/builds; not cryptographic.
static inline uint32_t fnv1a32(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t h = 2166136261u;
    while (len--) h = (h ^ *p++) * 16777619u;
    return h;
}
