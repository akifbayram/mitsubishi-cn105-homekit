#pragma once

// M5Stack NanoC6 (ESP32-C6)
// Grove connector (HY2.0-4P): GPIO2=RX, GPIO1=TX
// Onboard WS2812 RGB LED: data=GPIO20, power=GPIO19
// Onboard blue LED: GPIO7
// BOOT button: GPIO9 (active-low)

#define BOARD_NAME              "M5Stack NanoC6"

// CN105 UART (Grove connector)
#define PIN_CN105_RX            2
#define PIN_CN105_TX            1
#define CN105_UART_NUM          UART_NUM_1

// Status LEDs
#define PIN_LED_DATA            20      // WS2812 RGB data
#define PIN_LED_ENABLE          19      // WS2812 power enable
#define HAS_NEOPIXEL            1
#define PIN_BLUE_LED            7       // Simple blue indicator LED

// Button (WiFi reset)
#define PIN_BUTTON              9
#define BUTTON_ACTIVE_LOW       1

// ESP32-C6 specifics
#define UART_NEEDS_RX_PULLUP    1
#define UART_USE_XTAL_CLK       1
