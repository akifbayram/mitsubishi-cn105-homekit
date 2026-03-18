#pragma once

// ESP32-S3-DevKitC-1 (with onboard WS2812)
// Common wiring: GPIO18=RX, GPIO17=TX (UART1)
// Onboard WS2812: GPIO48 (no enable pin)
// BOOT button: GPIO0 (active-low)

#define BOARD_NAME              "ESP32-S3 DevKit"

// CN105 UART
#define PIN_CN105_RX            18
#define PIN_CN105_TX            17
#define CN105_UART_NUM          UART_NUM_1

// Status LED (onboard WS2812)
#define PIN_LED_DATA            48
#define PIN_LED_ENABLE          -1
#define HAS_NEOPIXEL            1

// Button
#define PIN_BUTTON              0
#define BUTTON_ACTIVE_LOW       1

// ESP32-S3
#define UART_NEEDS_RX_PULLUP    0
#define UART_USE_XTAL_CLK       0
