#include "Arduino.h"
#include "logging.h"

// ── Controllable clock ──────────────────────────────────────────────────────
uint32_t stub_millis = 0;

// ── Logging globals ────────────────────────────────────────────────────────
LogLevel currentLogLevel = LOG_LEVEL_INFO;
LogHookFn logHook = nullptr;

// ── Dummy Serial ────────────────────────────────────────────────────────────
DummySerial Serial;

// ── neopixelWrite recorder ──────────────────────────────────────────────────
std::vector<NeopixelCall> neopixel_log;

void neopixelWrite(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
    neopixel_log.push_back({pin, r, g, b});
}
