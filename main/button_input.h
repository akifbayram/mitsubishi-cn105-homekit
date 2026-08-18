#pragma once
// Pure button mechanics — NO ESP-IDF includes (host-testable, test/button_input).
//
// This module knows debounce, what separates a click from a long press, and
// when a click burst has settled. It deliberately does NOT know what any hold
// DURATION means: the 2 s Link-pair / 7 s warn / 10 s WiFi-erase bands stay in
// wifi_recovery.cpp, which is the only place they have ever lived.

#include <stdint.h>

constexpr uint32_t BTN_DEBOUNCE_MS  = 25;    // contact settle
constexpr uint32_t BTN_CLICK_MAX_MS = 600;   // a press shorter than this is a click
constexpr uint32_t BTN_BURST_GAP_MS = 400;   // quiet after the last release settles a burst
constexpr uint32_t BTN_STARVE_MS    = 250;   // poll gap that invalidates all interval timing

enum ButtonEvent : uint8_t {
    BTN_EV_NONE,      // nothing settled this tick
    BTN_EV_CLICK,     // a settled click burst; count in .clicks
    BTN_EV_RELEASE,   // a long press ended; .heldMs carries its duration
};

struct ButtonOut {
    ButtonEvent ev;
    uint8_t     clicks;
    uint32_t    heldMs;   // live hold duration while pressed; final duration on
                          // BTN_EV_RELEASE; 0 otherwise
    bool        pressed;  // debounced level
};

struct ButtonInput {
    ButtonOut update(bool rawPressed, uint32_t nowMs) {
        ButtonOut out = {BTN_EV_NONE, 0, 0, _stable};

        // A starved poll — the main loop stalled on an OTA upload or HomeKit
        // pairing — makes every measured interval meaningless. Drop the burst
        // rather than read the stall as extra clicks.
        if (_started && (uint32_t)(nowMs - _lastCall) > BTN_STARVE_MS) {
            _burst = 0;
        }
        _lastCall = nowMs;
        _started  = true;

        // Debounce: adopt the raw level only once it has held steady.
        if (rawPressed != _raw) { _raw = rawPressed; _rawSince = nowMs; }
        if (_raw != _stable && (uint32_t)(nowMs - _rawSince) >= BTN_DEBOUNCE_MS) {
            _stable = _raw;
            if (_stable) {
                _pressStart = nowMs;
            } else {
                uint32_t held = (uint32_t)(nowMs - _pressStart);
                if (held < BTN_CLICK_MAX_MS) {
                    if (_burst < 255) _burst++;
                    _burstDeadline = nowMs + BTN_BURST_GAP_MS;
                } else {
                    _burst     = 0;          // a long press is never part of a burst
                    out.ev     = BTN_EV_RELEASE;
                    out.heldMs = held;
                }
            }
        }

        out.pressed = _stable;
        if (_stable) out.heldMs = (uint32_t)(nowMs - _pressStart);

        // The burst settles once the quiet gap elapses with the button up.
        if (!_stable && _burst > 0 && (int32_t)(nowMs - _burstDeadline) >= 0) {
            out.ev     = BTN_EV_CLICK;
            out.clicks = _burst;
            _burst     = 0;
        }
        return out;
    }

private:
    bool     _raw = false, _stable = false, _started = false;
    uint32_t _rawSince = 0, _pressStart = 0, _lastCall = 0, _burstDeadline = 0;
    uint8_t  _burst = 0;
};
