# Adding a Custom Board

Two options for adding a board not included in the default set.

## Option A — Profile Header (Recommended for Reuse)

1. Create `main/boards/board_myboard.h`:

```c
#pragma once

#define BOARD_NAME              "My Board"

// CN105 UART — set to your RX/TX GPIOs
#define PIN_CN105_RX            16
#define PIN_CN105_TX            17
#define CN105_UART_NUM          UART_NUM_2   // UART_NUM_1 or UART_NUM_2

// Status LED — set to -1 to disable
#define PIN_LED_DATA            -1
#define PIN_LED_ENABLE          -1
#define HAS_NEOPIXEL            0

// Button — set to -1 to disable
#define PIN_BUTTON              0
#define BUTTON_ACTIVE_LOW       1

// Platform quirks
#define UART_NEEDS_RX_PULLUP    0   // 1 for ESP32-C6/C3 (floating GPIOs)
#define UART_USE_XTAL_CLK       0   // 1 for ESP32-C6/C3 (precise 2400 baud)
```

2. Add to `main/board_profile.h`:

```c
#elif defined(BOARD_PROFILE_MYBOARD)
    #include "boards/board_myboard.h"
```

3. Add target mapping in `main/CMakeLists.txt` (or pass as a build flag):

```bash
idf.py -DBOARD_PROFILE_MYBOARD=1 build
```

## Option B — Inline Overrides (Quick, No Header File)

```bash
idf.py -DBOARD_PROFILE_CUSTOM=1 -DPIN_CN105_RX=16 -DPIN_CN105_TX=17 build
```

Any macros not set fall back to safe defaults (LED and button disabled, `UART_NUM_1`). See `main/board_profile.h` for the full list.
