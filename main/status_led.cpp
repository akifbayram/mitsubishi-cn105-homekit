#include "status_led.h"
#include "logging.h"
#include "esp_utils.h"

static const char *TAG = "led";

#if PIN_LED_DATA >= 0

#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr uint8_t MAX_BRIGHT = 30;

static const char* stateName(LEDState s) {
    switch (s) {
        case SLED_OFF:                return "OFF";
        case SLED_BOOT:               return "BOOT";
        case SLED_CN105_DISCONNECTED: return "CN105_DISC";
        case SLED_WIFI_DISCONNECTED:  return "WIFI_DISC";
        case SLED_ERROR_CODE:         return "ERROR";
        case SLED_OTA:                return "OTA";
        case SLED_PAIR_LISTEN:        return "PAIR_LISTEN";
        case SLED_BLE_PAIR_LISTEN:    return "BLE_PAIR_LISTEN";
        case SLED_RESULT_OK:          return "RESULT_OK";
        case SLED_RESULT_FAIL:        return "RESULT_FAIL";
        case SLED_UNPAIR:             return "UNPAIR";
        case SLED_PORTAL:             return "PORTAL";
        case SLED_WIFI_TRIAL:         return "WIFI_TRIAL";
        case SLED_SAFE_MODE:          return "SAFE_MODE";
        case SLED_IDENTIFY:           return "IDENTIFY";
        case SLED_BTN_PAIR:           return "BTN_PAIR";
        case SLED_BTN_WIPE:           return "BTN_WIPE";
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
    LOG_INFO("RGB on GPIO%d (pwr GPIO%d), blue on GPIO%d",
             _pin, _enablePin, _bluePin);
}

void StatusLED::setState(LEDState state) {
    // A requestHold() override (set from another task) wins until it expires.
    // Which states outrank a hold — and whether they cancel it or merely
    // bypass it — is policy, so it lives in sled_policy.h alongside the
    // priority chain (and its host tests).
    if (_holdUntil) {
        if (sled_cancels_hold(state) || (int32_t)(_holdUntil - uptime_ms()) <= 0) {
            _holdUntil = 0;
        } else if (!sled_bypasses_hold(state)) {
            state = _holdState;
        }
    }
    if (state == _state) return;
    LOG_INFO("%s -> %s", stateName(_state), stateName(state));
    _state = state;
    uint32_t now = uptime_ms();
    _stateStart = now;
    _lastToggle = now;
    _rgbOn = false;

    switch (state) {
        case SLED_OFF:
            off();
            break;
        case SLED_BOOT:
            setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);       // white solid
            break;
        case SLED_CN105_DISCONNECTED:
            setColor(MAX_BRIGHT, 0, 0);                          // red solid
            break;
        case SLED_WIFI_DISCONNECTED:
            setColor(0, 0, MAX_BRIGHT);                          // blue solid
            break;
        case SLED_RESULT_OK:
            setColor(0, MAX_BRIGHT, 0);                          // green solid
            break;
        case SLED_BTN_PAIR:
            setColor(MAX_BRIGHT, 0, MAX_BRIGHT);                 // purple solid
            break;
        case SLED_ERROR_CODE:
        case SLED_RESULT_FAIL:
        case SLED_BTN_WIPE:
            _rgbOn = true;
            setColor(MAX_BRIGHT, 0, 0);                          // red, blinks in loop()
            break;
        case SLED_WIFI_TRIAL:
            _rgbOn = true;
            setColor(0, 0, MAX_BRIGHT);                          // blue, blinks in loop()
            break;
        case SLED_IDENTIFY:
            _rgbOn = true;
            setColor(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);        // white, blinks in loop()
            break;
        case SLED_UNPAIR:
            _rgbOn = true;
            setColor(MAX_BRIGHT, MAX_BRIGHT / 2, 0);             // orange, blinks in loop()
            break;
        case SLED_OTA:
        case SLED_PAIR_LISTEN:
        case SLED_BLE_PAIR_LISTEN:
        case SLED_PORTAL:
        case SLED_SAFE_MODE:
            // pulses — animated in loop(); dark until first tick
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
    LOG_INFO("Blue %s (WiFi %s)", ledOn ? "ON" : "OFF",
             connected ? "connected" : "disconnected");
}

// 2 s triangle wave, 0..MAX_BRIGHT
uint8_t StatusLED::pulseLevel(uint32_t stateAge) {
    uint32_t phase = stateAge % 2000;
    if (phase < 1000) return (uint8_t)((phase * MAX_BRIGHT) / 1000);
    return (uint8_t)(((2000 - phase) * MAX_BRIGHT) / 1000);
}

// 200 ms on/off fast blink in the given color
void StatusLED::blinkTick(uint32_t now, uint8_t r, uint8_t g, uint8_t b) {
    if (now - _lastToggle < 200) return;
    _lastToggle = now;
    _rgbOn = !_rgbOn;
    if (_rgbOn) setColor(r, g, b);
    else off();
}

// 2 s breathe between dark and the given full-brightness color, refreshed at
// ~50 Hz. Components scale together, so the hue holds across the ramp.
void StatusLED::pulseTick(uint32_t now, uint32_t stateAge,
                          uint8_t r, uint8_t g, uint8_t b) {
    if (now - _lastToggle < 20) return;
    _lastToggle = now;
    uint8_t v = pulseLevel(stateAge);
    setColor((uint8_t)((r * v) / MAX_BRIGHT),
             (uint8_t)((g * v) / MAX_BRIGHT),
             (uint8_t)((b * v) / MAX_BRIGHT));
}

void StatusLED::loop() {
    uint32_t now = uptime_ms();
    uint32_t stateAge = now - _stateStart;

    switch (_state) {
        // Solids — set once in setState()
        case SLED_OFF:
        case SLED_BOOT:
        case SLED_CN105_DISCONNECTED:
        case SLED_WIFI_DISCONNECTED:
        case SLED_RESULT_OK:
        case SLED_BTN_PAIR:
            break;

        // Fast blinks (200 ms)
        case SLED_ERROR_CODE:
        case SLED_RESULT_FAIL:
        case SLED_BTN_WIPE:
            blinkTick(now, MAX_BRIGHT, 0, 0);                    // red
            break;
        case SLED_WIFI_TRIAL:
            blinkTick(now, 0, 0, MAX_BRIGHT);                    // blue
            break;
        case SLED_IDENTIFY:
            blinkTick(now, MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);  // white
            break;
        case SLED_UNPAIR:
            blinkTick(now, MAX_BRIGHT, MAX_BRIGHT / 2, 0);       // orange
            break;

        // Slow pulses (2 s triangle, ~50 Hz refresh)
        case SLED_OTA:
            pulseTick(now, stateAge, MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT);  // white
            break;
        case SLED_PAIR_LISTEN:
            pulseTick(now, stateAge, MAX_BRIGHT, 0, MAX_BRIGHT);           // purple
            break;
        case SLED_BLE_PAIR_LISTEN:
            pulseTick(now, stateAge, 0, MAX_BRIGHT, MAX_BRIGHT);           // cyan
            break;
        case SLED_PORTAL:
            pulseTick(now, stateAge, 0, 0, MAX_BRIGHT);                    // blue
            break;
        case SLED_SAFE_MODE:
            pulseTick(now, stateAge, MAX_BRIGHT, (MAX_BRIGHT * 2) / 3, 0); // yellow
            break;
    }
}

void StatusLED::requestHold(LEDState state, uint32_t ms) {
    // Two plain 32-bit stores — state first so a concurrent setState() that
    // sees the armed deadline always reads the intended hold state.
    _holdState = state;
    _holdUntil = uptime_ms() + ms;
}

void StatusLED::holdBlocking(LEDState state, uint32_t ms) {
    setState(state);
    uint32_t until = uptime_ms() + ms;
    while ((int32_t)(until - uptime_ms()) > 0) {
        loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    off();
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
