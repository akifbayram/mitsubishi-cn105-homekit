#include "status_led.h"
#include "logging.h"
#include "esp_utils.h"

static const char *TAG = "led";

#if PIN_LED_DATA >= 0

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <led_strip.h>

static constexpr uint8_t MAX_BRIGHT = 30;

static const char* stateName(LEDState s) {
    switch (s) {
        case SLED_OFF:                return "OFF";
        case SLED_BOOT:               return "BOOT";
        case SLED_CN105_DISCONNECTED: return "CN105_DISC";
        case SLED_WIFI_DISCONNECTED:  return "WIFI_DISC";
        case SLED_ERROR_CODE:         return "ERROR";
        case SLED_OTA:                return "OTA";
        default:                      return "?";
    }
}

StatusLED::StatusLED(uint8_t pin, int8_t enablePin, int8_t bluePin)
    : _pin(pin), _enablePin(enablePin), _bluePin(bluePin) {}

void StatusLED::begin() {
    // Reset all LED pins to clear sleep isolation (required on ESP32-C6)
    gpio_reset_pin((gpio_num_t)_pin);
    if (_enablePin >= 0) gpio_reset_pin((gpio_num_t)_enablePin);
    if (_bluePin >= 0)   gpio_reset_pin((gpio_num_t)_bluePin);

    // Blue indicator LED
    if (_bluePin >= 0) {
        gpio_set_direction((gpio_num_t)_bluePin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)_bluePin, 0);
    }

    // WS2812 power enable — set HIGH and leave it (per M5Stack reference design)
    if (_enablePin >= 0) {
        gpio_set_direction((gpio_num_t)_enablePin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)_enablePin, 1);
        esp_rom_delay_us(350);  // WS2812 needs ≥280μs after power-on
    }

    // Configure RMT-based WS2812 driver
    led_strip_config_t strip_config = {
        .strip_gpio_num = _pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols = 0,
        .flags = { .with_dma = false }
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &_strip);
    if (err != ESP_OK) {
        LOG_ERROR("led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        _strip = nullptr;
        return;
    }

    off();
    LOG_INFO("[LED] RGB on GPIO%d (pwr GPIO%d), blue on GPIO%d",
             _pin, _enablePin, _bluePin);
}

void StatusLED::setState(LEDState state) {
    if (state == _state) return;
    LOG_INFO("[LED] %s -> %s", stateName(_state), stateName(state));
    _state = state;
    uint32_t now = uptime_ms();
    _stateStart = now;
    _lastToggle = now;
    _rgbOn = false;

    switch (state) {
        case SLED_OFF:
            off();
            break;
        case SLED_CN105_DISCONNECTED:
            setColor(MAX_BRIGHT, 0, 0);  // red steady
            break;
        case SLED_WIFI_DISCONNECTED:
            setColor(0, 0, MAX_BRIGHT);  // blue steady
            break;
        case SLED_BOOT:
            _rgbOn = true;
            setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);  // white on immediately
            break;
        case SLED_ERROR_CODE:
            _rgbOn = true;
            setColor(MAX_BRIGHT, 0, 0);  // red on immediately
            break;
        default:
            break;
    }
}

void StatusLED::setWifi(bool connected) {
    if (_bluePin < 0) return;
    bool ledOn = !connected;  // Blue LED ON when WiFi is down
    if (ledOn == _wifiOn) return;
    _wifiOn = ledOn;
    gpio_set_level((gpio_num_t)_bluePin, ledOn ? 1 : 0);
    LOG_INFO("[LED] Blue %s (WiFi %s)", ledOn ? "ON" : "OFF",
             connected ? "connected" : "disconnected");
}

void StatusLED::loop() {
    uint32_t now = uptime_ms();
    uint32_t elapsed = now - _lastToggle;
    uint32_t stateAge = now - _stateStart;

    switch (_state) {
        case SLED_OFF:
        case SLED_CN105_DISCONNECTED:
        case SLED_WIFI_DISCONNECTED:
            break;

        case SLED_BOOT: {
            if (elapsed >= 500) {
                _lastToggle = now;
                _rgbOn = !_rgbOn;
                if (_rgbOn) setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);
                else off();
            }
            break;
        }

        case SLED_ERROR_CODE: {
            if (elapsed >= 200) {
                _lastToggle = now;
                _rgbOn = !_rgbOn;
                if (_rgbOn) setColor(MAX_BRIGHT, 0, 0);
                else off();
            }
            break;
        }

        case SLED_OTA: {
            if (elapsed < 20) break;  // Rate-limit: ~50 Hz
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
    if (!_strip) return;
    led_strip_set_pixel(_strip, 0, r, g, b);
    led_strip_refresh(_strip);
}

void StatusLED::off() {
    if (_strip) {
        led_strip_clear(_strip);
    }
    _rgbOn = false;
}

#endif // PIN_LED_DATA >= 0
