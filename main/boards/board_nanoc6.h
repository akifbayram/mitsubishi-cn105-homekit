#pragma once

// M5Stack NanoC6 (ESP32-C6)
// Grove connector (HY2.0-4P): GPIO2=RX, GPIO1=TX
// Onboard WS2812 RGB LED: data=GPIO20, enable=GPIO19
// BOOT button: GPIO9 (active-low)

#define BOARD_NAME              "M5Stack NanoC6"

// CN105 UART (Grove connector)
#define PIN_CN105_RX            2
#define PIN_CN105_TX            1
#define CN105_UART_NUM          UART_NUM_1

// Status LED (onboard WS2812)
#define PIN_LED_DATA            20
#define PIN_LED_ENABLE          19
#define HAS_NEOPIXEL            1

// Button (WiFi reset)
#define PIN_BUTTON              9
#define BUTTON_ACTIVE_LOW       1

// ESP32-C6 specifics
#define UART_NEEDS_RX_PULLUP    1
#define UART_USE_XTAL_CLK       1
