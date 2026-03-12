#pragma once

// ── Board Profile System ────────────────────────────────────────────────────
// Hardware configuration selected at build time via PlatformIO environment.
//
// Predefined boards:
//   pio run -e nanoc6          M5Stack NanoC6 (ESP32-C6) — default
//   pio run -e esp32-devkit    Generic ESP32 DevKit v1
//   pio run -e esp32s3-devkit  ESP32-S3-DevKitC-1
//   pio run -e esp32c3-mini    ESP32-C3 SuperMini / XIAO
//   pio run -e m5atoms3-lite   M5Stack AtomS3 Lite
//
// Custom board (override individual pins, no profile header needed):
//   build_flags = -DBOARD_PROFILE_CUSTOM -DPIN_CN105_RX=16 -DPIN_CN105_TX=17 ...
//
// To add a new predefined board:
//   1. Create include/boards/board_<name>.h (see existing profiles for format)
//   2. Add #elif in the selector below
//   3. Add [env:<name>] in platformio.ini

// ── Board profile selector ──────────────────────────────────────────────────
#if defined(BOARD_PROFILE_NANOC6)
    #include "boards/board_nanoc6.h"
#elif defined(BOARD_PROFILE_ESP32_DEVKIT)
    #include "boards/board_esp32_devkit.h"
#elif defined(BOARD_PROFILE_ESP32S3_DEVKIT)
    #include "boards/board_esp32s3_devkit.h"
#elif defined(BOARD_PROFILE_ESP32C3_MINI)
    #include "boards/board_esp32c3_mini.h"
#elif defined(BOARD_PROFILE_M5ATOMS3_LITE)
    #include "boards/board_m5atoms3_lite.h"
#elif defined(BOARD_PROFILE_CUSTOM)
    // All pins defined via -D flags; defaults below fill any gaps
#else
    // No profile specified — default to NanoC6 for backward compatibility
    #include "boards/board_nanoc6.h"
#endif

// ── Defaults for any missing definitions ────────────────────────────────────

#ifndef BOARD_NAME
#define BOARD_NAME              "Unknown Board"
#endif

// CN105 UART
#ifndef PIN_CN105_RX
#define PIN_CN105_RX            2
#endif
#ifndef PIN_CN105_TX
#define PIN_CN105_TX            1
#endif
#ifndef CN105_UART_NUM
#define CN105_UART_NUM          UART_NUM_1
#endif

// Status LED (-1 = no LED)
#ifndef PIN_LED_DATA
#define PIN_LED_DATA            -1
#endif
#ifndef PIN_LED_ENABLE
#define PIN_LED_ENABLE          -1
#endif
#ifndef HAS_NEOPIXEL
#define HAS_NEOPIXEL            0
#endif

// Button (-1 = no button)
#ifndef PIN_BUTTON
#define PIN_BUTTON              -1
#endif
#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW       1
#endif

// USB debug output
#ifndef USE_HWCDC_DEBUG
#define USE_HWCDC_DEBUG         0
#endif

// UART hardware quirks
#ifndef UART_NEEDS_RX_PULLUP
#define UART_NEEDS_RX_PULLUP    0
#endif
#ifndef UART_USE_XTAL_CLK
#define UART_USE_XTAL_CLK       0
#endif
