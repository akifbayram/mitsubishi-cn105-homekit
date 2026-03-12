#pragma once

// Generic ESP32 DevKit v1 / NodeMCU-32S
// Common wiring: GPIO16=RX2, GPIO17=TX2
// No onboard addressable LED
// BOOT button: GPIO0 (active-low)

#define BOARD_NAME              "ESP32 DevKit"

// CN105 UART
#define PIN_CN105_RX            16
#define PIN_CN105_TX            17
#define CN105_UART_NUM          UART_NUM_2

// No onboard NeoPixel
#define PIN_LED_DATA            -1
#define PIN_LED_ENABLE          -1
#define HAS_NEOPIXEL            0

// Button
#define PIN_BUTTON              0
#define BUTTON_ACTIVE_LOW       1

// Classic ESP32
#define USE_HWCDC_DEBUG         0
#define UART_NEEDS_RX_PULLUP    0
#define UART_USE_XTAL_CLK       0
