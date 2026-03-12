#pragma once

// ESP32-C3 SuperMini / Seeed XIAO ESP32-C3
// Common wiring: GPIO20=RX, GPIO21=TX
// No onboard addressable LED
// BOOT button: GPIO9 (active-low)

#define BOARD_NAME              "ESP32-C3 Mini"

// CN105 UART
#define PIN_CN105_RX            20
#define PIN_CN105_TX            21
#define CN105_UART_NUM          UART_NUM_1

// No onboard NeoPixel
#define PIN_LED_DATA            -1
#define PIN_LED_ENABLE          -1
#define HAS_NEOPIXEL            0

// Button
#define PIN_BUTTON              9
#define BUTTON_ACTIVE_LOW       1

// ESP32-C3: USBSerial not HWCDC, floating GPIOs, XTAL clock recommended
#define USE_HWCDC_DEBUG         0
#define UART_NEEDS_RX_PULLUP    1
#define UART_USE_XTAL_CLK       1
