#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"

// ── Log Levels ────────────────────────────────────────────────────────────────
// Values must remain stable for NVS backward compatibility.
enum LogLevel : uint8_t {
    LOG_LEVEL_ERROR = 0,   // Errors only
    LOG_LEVEL_WARN  = 1,   // + warnings
    LOG_LEVEL_INFO  = 2,   // + connection events, state changes
    LOG_LEVEL_DEBUG = 3,   // + packet hex dumps, sync details
};

// Runtime-configurable log level (default: INFO)
extern LogLevel currentLogLevel;

// Pop one queued log line for WebSocket streaming (oldest first). When the
// ring overflowed since the last report, returns a "[log ring: N line(s)
// dropped]" marker instead of a line. Returns the copied length (NUL always
// written), 0 when nothing is queued. Single consumer: the main task
// (WebUI::loop) — see the threading contract in logging.cpp.
size_t logging_drain(char *out, size_t cap);

// ── Initialisation ───────────────────────────────────────────────────────────
// Install the custom vprintf handler that forwards logs to the WebSocket hook.
void logging_init();

// Convert our LogLevel enum to the ESP-IDF esp_log_level_t.
esp_log_level_t log_level_to_esp(LogLevel level);

// Apply the current LogLevel globally (sets ESP-IDF log level for all tags).
void logging_set_level(LogLevel level);

// ── Log Macros ────────────────────────────────────────────────────────────────
// Each source file must define a TAG:
//   static const char *TAG = "myfile";
// Then use: LOG_INFO("Connected to %s", ssid);

#define LOG_ERROR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
