#pragma once
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

// GPIO shims — prefer gpio_set_direction/gpio_set_level directly.
// WARNING: INPUT_PULLUP cannot be correctly shimmed. Arduino's
// pinMode(pin, INPUT_PULLUP) sets both direction AND pull-up in one call.
// In ESP-IDF, you must call gpio_set_direction() + gpio_set_pull_mode()
// separately. Replace all INPUT_PULLUP usage with direct ESP-IDF calls.
#define OUTPUT GPIO_MODE_OUTPUT
#define INPUT GPIO_MODE_INPUT
#define HIGH 1
#define LOW 0
