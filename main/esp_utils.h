#pragma once
#include <esp_timer.h>

static inline uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
