#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

// ── Controllable millis() ──────────────────────────────────────────────────
extern uint32_t stub_millis;
inline uint32_t millis() { return stub_millis; }

// ── Arduino macros ─────────────────────────────────────────────────────────
#define constrain(x, lo, hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#define PROGMEM

// ── GPIO constants ─────────────────────────────────────────────────────────
#define OUTPUT 1
#define INPUT  0
#define HIGH   1
#define LOW    0
#define INPUT_PULLUP 2

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline void delay(uint32_t) {}

// ── neopixelWrite — records calls for LED test assertions ──────────────────
struct NeopixelCall { uint8_t pin; uint8_t r; uint8_t g; uint8_t b; };
extern std::vector<NeopixelCall> neopixel_log;
void neopixelWrite(uint8_t pin, uint8_t r, uint8_t g, uint8_t b);

// ── Dummy Serial class ────────────────────────────────────────────────────
class DummySerial {
public:
    int printf(const char*, ...) { return 0; }
    int println() { return 0; }
    int println(const char*) { return 0; }
};
extern DummySerial Serial;
