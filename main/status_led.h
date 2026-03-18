#pragma once

#include <cstdint>
#include "board_profile.h"

enum LEDState {
    SLED_OFF,                // Normal operation — LED dark
    SLED_BOOT,               // Startup — white quick blink (500ms)
    SLED_WIFI_CONNECTING,    // WiFi not connected — orange steady
    SLED_WIFI_CONNECTED,     // Brief green flash (1s), then caller sets SLED_OFF
    SLED_CN105_DISCONNECTED, // CN105 UART lost — red steady
    SLED_ERROR_CODE,         // AC error code (non-0x80) — red fast blink (200ms)
    SLED_OTA                 // Firmware upload — blue slow pulse (~2s)
};

#if PIN_LED_DATA >= 0

#include <led_strip.h>

class StatusLED {
public:
    explicit StatusLED(uint8_t pin, int8_t enablePin = -1);
    void begin();
    void setState(LEDState state);
    LEDState getState() const { return _state; }
    void loop();

private:
    uint8_t              _pin;
    int8_t               _enablePin;
    LEDState             _state      = SLED_OFF;
    bool                 _ledOn      = false;
    bool                 _enableHigh = false;
    uint32_t             _lastToggle = 0;
    uint32_t             _stateStart = 0;
    led_strip_handle_t   _strip      = nullptr;

    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void off();
};

#endif // PIN_LED_DATA >= 0
