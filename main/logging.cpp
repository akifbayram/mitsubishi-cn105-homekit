#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ── Globals ──────────────────────────────────────────────────────────────────
LogLevel currentLogLevel = LOG_LEVEL_INFO;
LogHookFn logHook = nullptr;

// ── Custom vprintf handler ───────────────────────────────────────────────────
// Installed via esp_log_set_vprintf(). Every ESP_LOG* call passes through here.
// We format once, write to stdout, and optionally forward to the WebSocket hook.
static int log_hook_vprintf(const char *fmt, va_list args) {
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len < 0) {
        return len;
    }

    // Clamp to buffer size (vsnprintf may return more than sizeof(buf)-1)
    size_t written = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;

    // Console output
    fputs(buf, stdout);

    // Forward to WebSocket log hook if registered
    if (logHook) {
        logHook(buf, written);
    }

    return len;
}

// ── Level conversion ─────────────────────────────────────────────────────────
esp_log_level_t log_level_to_esp(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return ESP_LOG_ERROR;
        case LOG_LEVEL_WARN:  return ESP_LOG_WARN;
        case LOG_LEVEL_INFO:  return ESP_LOG_INFO;
        case LOG_LEVEL_DEBUG: return ESP_LOG_DEBUG;
        default:              return ESP_LOG_INFO;
    }
}

void logging_set_level(LogLevel level) {
    currentLogLevel = level;
    esp_log_level_set("*", log_level_to_esp(level));
}

// ── Init ─────────────────────────────────────────────────────────────────────
void logging_init() {
    // Set global log level from our stored preference
    esp_log_level_set("*", log_level_to_esp(currentLogLevel));

    // Install custom handler to intercept all ESP_LOG* output
    esp_log_set_vprintf(log_hook_vprintf);
}
