#include "logging.h"
#include "log_ring.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── Globals ──────────────────────────────────────────────────────────────────
LogLevel currentLogLevel = LOG_LEVEL_INFO;

// ── Log line ring ────────────────────────────────────────────────────────────
// Producers: the vprintf hook, on WHATEVER task logged. Consumer: the main
// task, via logging_drain() from WebUI::loop().
//
// THREADING CONTRACT
//  - s_fmtMux guards the hook's static format buffer, s_ring and
//    s_dropReported; every reader and
//    writer takes it. A holder only formats, fputs, and memcpys — fputs is
//    bounded (USB-Serial-JTAG TX fail-fasts ≤50 ms once when the host
//    stops reading, then drops), so waits stay short.
//  - The hook must do NOTHING beyond format + console + ring push. With
//    CONFIG_LOG_VERSION_1 it runs UNLOCKED on the calling task — including
//    2 KB-stack tasks (Tmr Svc: captured "Stack canary watchpoint
//    triggered" panic) and the WiFi task (a socket send here can stall it
//    into beacon loss). Socket I/O lives exclusively in the main-task
//    drain path.
//  - Never call LOG_* while holding s_fmtMux (it is not recursive).
static LogRing s_ring;
static SemaphoreHandle_t s_fmtMux = nullptr;
static uint32_t s_dropReported = 0;

// ── Custom vprintf handler ───────────────────────────────────────────────────
// Installed via esp_log_set_vprintf(). Formats once into a shared buffer
// (static: 256 B of stack would overflow constrained logging tasks), writes
// the console, and queues the line for the WebSocket drain.
static int log_hook_vprintf(const char *fmt, va_list args) {
    if (!s_fmtMux) return vprintf(fmt, args);   // before logging_init(): console only

    xSemaphoreTake(s_fmtMux, portMAX_DELAY);
    static char buf[256];                        // guarded by s_fmtMux
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t written = (len < (int)sizeof(buf)) ? (size_t)len : sizeof(buf) - 1;
        fputs(buf, stdout);
        s_ring.push(buf, written);
    }
    xSemaphoreGive(s_fmtMux);
    return len;
}

size_t logging_drain(char *out, size_t cap) {
    if (!s_fmtMux || !out || cap == 0) return 0;
    xSemaphoreTake(s_fmtMux, portMAX_DELAY);
    size_t len;
    uint32_t lost = s_ring.dropped() - s_dropReported;
    if (lost > 0) {
        s_dropReported += lost;
        int n = snprintf(out, cap, "[log ring: %lu line(s) dropped]",
                         (unsigned long)lost);
        len = (n > 0) ? ((n < (int)cap) ? (size_t)n : cap - 1) : 0;
    } else {
        len = s_ring.pop(out, cap);
    }
    xSemaphoreGive(s_fmtMux);
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

    if (!s_fmtMux) s_fmtMux = xSemaphoreCreateMutex();

    // Install custom handler to intercept all ESP_LOG* output
    esp_log_set_vprintf(log_hook_vprintf);
}
