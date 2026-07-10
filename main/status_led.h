#pragma once

#include <cstdint>
#include "board_profile.h"

// RGB LED states (WS2812)
enum LEDState {
    SLED_OFF,                // Normal operation — LED dark
    SLED_BOOT,               // Startup — white quick blink (500ms)
    SLED_CN105_DISCONNECTED, // CN105 UART lost — red steady
    SLED_WIFI_DISCONNECTED,  // WiFi lost — blue steady (WIFI_ON_RGB boards only)
    SLED_ERROR_CODE,         // AC error code (non-0x80) — red fast blink (200ms)
    SLED_OTA,                // Firmware upload — blue slow pulse (~2s)
    SLED_PAIR_LISTEN,        // Link pairing window open — purple slow pulse
    SLED_PAIR_OK,            // Pairing succeeded — solid green (held ~5s by caller)
    SLED_PAIR_FAIL,          // Pairing timed out — red fast blink (held ~3s by caller)
    SLED_UNPAIR              // Link forgotten — orange fast blink (held by caller before restart)
};

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
    // Blocking hold: drive `state` for `ms`, animating the LED, then turn it
    // off. For terminal indications shown right before an esp_restart() (e.g.
    // SLED_UNPAIR), where the main-loop LED evaluation won't get another tick.
    // MAIN TASK ONLY — it drives the (non-thread-safe) strip itself.
    void holdBlocking(LEDState state, uint32_t ms);
    // Non-blocking hold, callable from other tasks (e.g. httpd): setState()
    // substitutes `state` for whatever the main loop asks for until the
    // deadline passes. The main task keeps sole ownership of the strip.
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

    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void off();
};

extern StatusLED statusLED;   // defined in main.cpp

#endif // PIN_LED_DATA >= 0
