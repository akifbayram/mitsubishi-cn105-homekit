#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"

// ── Log Levels ────────────────────────────────────────────────────────────────
// Values match the original Arduino enum for NVS settings compatibility.
enum LogLevel : uint8_t {
    LOG_LEVEL_ERROR = 0,   // Errors only
    LOG_LEVEL_WARN  = 1,   // + warnings
    LOG_LEVEL_INFO  = 2,   // + connection events, state changes
    LOG_LEVEL_DEBUG = 3,   // + packet hex dumps, sync details
};

// Runtime-configurable log level (default: INFO)
extern LogLevel currentLogLevel;

// ── Log Hook (for WebSocket streaming) ────────────────────────────────────────
// Called by the custom vprintf handler with each formatted log line.
typedef void (*LogHookFn)(const char *msg, size_t len);
extern LogHookFn logHook;

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
