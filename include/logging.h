#pragma once

#include <Arduino.h>
#include <HWCDC.h>

// USB Serial/JTAG for debug logging (ESP32-C6 built-in USB)
extern HWCDC DebugLog;

// ── Log Levels ────────────────────────────────────────────────────────────────
enum LogLevel : uint8_t {
    LOG_LEVEL_ERROR = 0,   // Errors only
    LOG_LEVEL_WARN  = 1,   // + warnings
    LOG_LEVEL_INFO  = 2,   // + connection events, state changes
    LOG_LEVEL_DEBUG = 3,   // + packet hex dumps, sync details
};

// Runtime-configurable log level (default: INFO)
extern LogLevel currentLogLevel;

// ── Log Hook (for WebSocket streaming) ────────────────────────────────────────
typedef void (*LogHookFn)(const char *msg);
extern LogHookFn logHook;

// ── Log Macros ────────────────────────────────────────────────────────────────
// Usage: LOG_INFO("[CN105] Connected!"); LOG_DEBUG("[CN105] TX (%d bytes)", len);

#define LOG_ERROR(fmt, ...) do { if (currentLogLevel >= LOG_LEVEL_ERROR) { \
    DebugLog.printf(fmt "\n", ##__VA_ARGS__); \
    if (logHook) { char _lbuf[256]; snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__); logHook(_lbuf); } \
} } while(0)

#define LOG_WARN(fmt, ...) do { if (currentLogLevel >= LOG_LEVEL_WARN) { \
    DebugLog.printf(fmt "\n", ##__VA_ARGS__); \
    if (logHook) { char _lbuf[256]; snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__); logHook(_lbuf); } \
} } while(0)

#define LOG_INFO(fmt, ...) do { if (currentLogLevel >= LOG_LEVEL_INFO) { \
    DebugLog.printf(fmt "\n", ##__VA_ARGS__); \
    if (logHook) { char _lbuf[256]; snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__); logHook(_lbuf); } \
} } while(0)

#define LOG_DEBUG(fmt, ...) do { if (currentLogLevel >= LOG_LEVEL_DEBUG) { \
    DebugLog.printf(fmt "\n", ##__VA_ARGS__); \
    if (logHook) { char _lbuf[256]; snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__); logHook(_lbuf); } \
} } while(0)
