#pragma once
#include <stdint.h>
#include <stddef.h>
#include <esp_timer.h>

static inline uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// FNV-1a 32-bit content hash (setup-code derivation, CAPS fingerprint).
// Deterministic across boots/builds; not cryptographic.
static inline uint32_t fnv1a32(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t h = 2166136261u;
    while (len--) h = (h ^ *p++) * 16777619u;
    return h;
}
