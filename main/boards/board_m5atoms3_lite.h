#pragma once

// M5Stack AtomS3 Lite (ESP32-S3)
// Grove connector: GPIO2=SDA/RX, GPIO1=SCL/TX
// Onboard WS2812: GPIO35 (no enable pin)
// Button: GPIO41 (active-low)

#define BOARD_NAME              "M5Stack AtomS3 Lite"

// CN105 UART (Grove connector)
#define PIN_CN105_RX            2
#define PIN_CN105_TX            1
#define CN105_UART_NUM          UART_NUM_1

// Status LED (onboard WS2812)
#define PIN_LED_DATA            35
#define PIN_LED_ENABLE          -1
#define HAS_NEOPIXEL            1

// Button
#define PIN_BUTTON              41
#define BUTTON_ACTIVE_LOW       1

// ESP32-S3 has HWCDC
#define USE_HWCDC_DEBUG         1
#define UART_NEEDS_RX_PULLUP    1
#define UART_USE_XTAL_CLK       0
