#include "status_led.h"
#include "logging.h"

static constexpr uint8_t MAX_BRIGHT = 30;

static const char* stateName(LEDState s) {
    switch (s) {
        case SLED_OFF: return "OFF";
        case SLED_BOOT: return "BOOT";
        case SLED_WIFI_CONNECTING: return "WIFI_CONNECTING";
        case SLED_WIFI_CONNECTED: return "WIFI_CONNECTED";
        case SLED_CN105_DISCONNECTED: return "CN105_DISC";
        case SLED_ERROR_CODE: return "ERROR";
        case SLED_OTA: return "OTA";
        default: return "?";
    }
}

StatusLED::StatusLED(uint8_t pin, int8_t enablePin)
    : _pin(pin), _enablePin(enablePin) {}

void StatusLED::begin() {
    if (_enablePin >= 0) {
        pinMode(_enablePin, OUTPUT);
        digitalWrite(_enablePin, HIGH);
        _enableHigh = true;
        delay(1);  // Let LED chip power stabilize
    }
    off();
}

void StatusLED::setState(LEDState state) {
    if (state == _state) return;
    LOG_INFO("[LED] %s → %s", stateName(_state), stateName(state));
    _state = state;
    _stateStart = millis();
    _lastToggle = millis();
    _ledOn = false;

    switch (state) {
        case SLED_OFF:
            off();
            break;
        case SLED_WIFI_CONNECTING:
            setColor(MAX_BRIGHT, MAX_BRIGHT / 3, 0);  // orange
            break;
        case SLED_CN105_DISCONNECTED:
            setColor(MAX_BRIGHT, 0, 0);  // red steady
            break;
        case SLED_WIFI_CONNECTED:
            setColor(0, MAX_BRIGHT, 0);  // green flash
            break;
        case SLED_BOOT:
            _ledOn = true;
            setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);  // white on immediately
            break;
        case SLED_ERROR_CODE:
            _ledOn = true;
            setColor(MAX_BRIGHT, 0, 0);  // red on immediately
            break;
        default:
            break;
    }
}

void StatusLED::loop() {
    uint32_t now = millis();
    uint32_t elapsed = now - _lastToggle;
    uint32_t stateAge = now - _stateStart;

    switch (_state) {
        case SLED_OFF:
        case SLED_WIFI_CONNECTING:
        case SLED_CN105_DISCONNECTED:
            break;

        case SLED_BOOT: {
            if (elapsed >= 500) {
                _lastToggle = now;
                _ledOn = !_ledOn;
                if (_ledOn) setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);
                else off();
            }
            break;
        }

        case SLED_WIFI_CONNECTED: {
            if (stateAge >= 1000) {
                setState(SLED_OFF);
            }
            break;
        }

        case SLED_ERROR_CODE: {
            if (elapsed >= 200) {
                _lastToggle = now;
                _ledOn = !_ledOn;
                if (_ledOn) setColor(MAX_BRIGHT, 0, 0);
                else off();
            }
            break;
        }

        case SLED_OTA: {
            if (elapsed < 20) break;  // Rate-limit: update every 20ms (~50 Hz)
            _lastToggle = now;
            uint32_t phase = stateAge % 2000;
            uint8_t brightness;
            if (phase < 1000) {
                brightness = (uint8_t)((phase * MAX_BRIGHT) / 1000);
            } else {
                brightness = (uint8_t)(((2000 - phase) * MAX_BRIGHT) / 1000);
            }
            setColor(0, 0, brightness);
            break;
        }
    }
}

void StatusLED::setColor(uint8_t r, uint8_t g, uint8_t b) {
    if (_enablePin >= 0 && !_enableHigh) {
        digitalWrite(_enablePin, HIGH);
        _enableHigh = true;
    }
    neopixelWrite(_pin, r, g, b);
}

void StatusLED::off() {
    neopixelWrite(_pin, 0, 0, 0);
    if (_enablePin >= 0 && _enableHigh) {
        digitalWrite(_enablePin, LOW);
        _enableHigh = false;
    }
    _ledOn = false;
}
