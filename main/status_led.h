#pragma once

#include <cstdint>
#include "board_profile.h"
#include "sled_policy.h"

#if PIN_LED_DATA >= 0

#include <led_strip.h>

class StatusLED {
public:
    explicit StatusLED(uint8_t pin, int8_t enablePin = -1, int8_t bluePin = -1);
    void begin();
    void setState(LEDState state);
    LEDState getState() const { return _state; }
    void setWifi(bool connected);
    void loop();
    // Blocking hold: drive `state` for `ms`, animating the LED, then return
    // to SLED_OFF (dark). For terminal indications shown right before an
    // esp_restart() (e.g. SLED_UNPAIR), where the main-loop LED evaluation
    // won't get another tick. Outranks an armed requestHold() — it cancels
    // it, so the terminal indication is always the one displayed.
    // MAIN TASK ONLY — it drives the (non-thread-safe) strip itself.
    void holdBlocking(LEDState state, uint32_t ms);
    // Non-blocking hold, callable from other tasks (e.g. httpd): setState()
    // substitutes `state` for whatever the main loop asks for until the
    // deadline passes (or a holdBlocking() cancels it). The main task keeps
    // sole ownership of the strip.
    void requestHold(LEDState state, uint32_t ms);

private:
    uint8_t              _pin;
    int8_t               _enablePin;
    int8_t               _bluePin;
    LEDState             _state      = SLED_OFF;
    bool                 _rgbOn      = false;
    uint32_t             _lastToggle = 0;
    uint32_t             _stateStart = 0;
    led_strip_handle_t   _strip      = nullptr;
    bool                 _wifiOn     = false;  // Blue LED state
    volatile LEDState    _holdState  = SLED_OFF;  // requestHold() override
    volatile uint32_t    _holdUntil  = 0;         // 0 = no hold armed

    // Shared body of setState()/holdBlocking(); `blocking` marks the terminal
    // main-task path, which sled_hold_action() never substitutes.
    void applyState(LEDState state, bool blocking);
    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void off();
    void blinkTick(uint32_t now, uint8_t r, uint8_t g, uint8_t b);
    void pulseTick(uint32_t now, uint32_t stateAge, uint8_t r, uint8_t g, uint8_t b);
    static uint8_t pulseLevel(uint32_t stateAge);
};

extern StatusLED statusLED;   // defined in main.cpp

#endif // PIN_LED_DATA >= 0

// "Which box is this?" — flash the LED for a few seconds. Callers (HAP
// identify routines, POST /identify) need no LED knowledge and no board
// guard: on boards without an RGB LED this compiles to nothing.
inline void status_led_identify() {
#if PIN_LED_DATA >= 0
    statusLED.requestHold(SLED_IDENTIFY, 3000);
#endif
}
