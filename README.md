# Mitsubishi CN105 HomeKit Controller

Controls Mitsubishi mini split heat pumps via the CN105 serial connector, compatible with Apple Home through the HomeKit Accessory Protocol (HAP). No cloud, no bridge, no Home Assistant required.

<table>
  <tr>
    <td><img src="media/homekit.gif" height=400></td>
    <td><img src="media/webui.png" height=400></td>
  </tr>
  <tr>
    <td align="center"><em>HomeKit</em></td>
    <td align="center"><em>Web UI</em></td>
  </tr>
</table>

> [!CAUTION]
> **Use at your own risk.** This is an unofficial implementation based on the reverse-engineered CN105 serial protocol. It is not developed, endorsed, or supported by Mitsubishi Electric or Apple. Connecting third-party hardware to your heat pump may void its warranty. Not all units support every feature, and behavior may vary by model. The authors and contributors provide this software as-is, with no warranty or guarantee of any kind. 

## Requirements

### Hardware


<table>
  <tr>
    <td><img src="media/wiring.png" width=300></td>
  </tr>
  <tr>
    <td align="center"><em>M5Stack NanoC6 with Grove to CN105 connector</em></td>
  </tr>
</table>



| Component | Details |
|-----------|---------|
| **Microcontroller** | Any [supported board](#supported-boards) (default: M5Stack NanoC6) |
| **Connector** | Grove (HY2.0-4P) to CN105 cable (NanoC6) |
| **Heat pump** | Mitsubishi mini split with CN105 connector |

> **Note:** Most Mitsubishi ductless and ducted units manufactured after 2010 have a CN105 connector on the indoor unit's control board. For a list of known-compatible models, see the [MitsubishiCN105ESPHome supported units list](https://github.com/echavet/MitsubishiCN105ESPHome?tab=readme-ov-file#supported-mitsubishi-climate-units). Not all units support every feature (e.g., outside temperature, half-degree precision, wide vane control) — behavior varies by model.

### Software

- [PlatformIO](https://platformio.org/) (build system)
- Python 3 (for HTML embedding script)

### Tested Boards

| Board | PlatformIO env | Build command | Tested |
|-------|---------------|---------------|-------|
| M5Stack NanoC6 (ESP32-C6) | `nanoc6` | `pio run -e nanoc6` | ✅ |
| M5Stack AtomS3/AtomS3 Lite | `m5atoms3-lite` | `pio run -e m5atoms3-lite` | ✅ |
| Generic ESP32 DevKit v1 | `esp32-devkit` | `pio run -e esp32-devkit` | ❌ |
| ESP32-S3-DevKitC-1 | `esp32s3-devkit` | `pio run -e esp32s3-devkit` | ❌ |
| ESP32-C3 SuperMini / XIAO | `esp32c3-mini` | `pio run -e esp32c3-mini` | ❌ |

Board profiles define GPIO pins, LED, button, UART clock source, and debug output for each target. See `include/boards/` for details.

### Adding a Custom Board

**Option A — Profile header** (recommended for reuse):

1. Create `include/boards/board_myboard.h`:

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
#define USE_HWCDC_DEBUG         0   // 1 for ESP32-C6/S3 native USB
#define UART_NEEDS_RX_PULLUP    0   // 1 for ESP32-C6/C3 (floating GPIOs)
#define UART_USE_XTAL_CLK       0   // 1 for ESP32-C6/C3 (precise 2400 baud)
```

2. Add to `include/board_profile.h`:

```c
#elif defined(BOARD_PROFILE_MYBOARD)
    #include "boards/board_myboard.h"
```

3. Add to `platformio.ini`:

```ini
[env:myboard]
platform = <your platform URL>
board = <your PlatformIO board ID>
extends = common
build_flags =
    -DBOARD_PROFILE_MYBOARD
```

**Option B — Inline overrides** (quick, no header file):

```ini
[env:myboard]
platform = <your platform URL>
board = <your PlatformIO board ID>
extends = common
build_flags =
    -DBOARD_PROFILE_CUSTOM
    -DPIN_CN105_RX=16
    -DPIN_CN105_TX=17
```

Any macros not set fall back to safe defaults (LED and button disabled, `UART_NUM_1`). See `include/board_profile.h` for the full list.

## Wiring

Connect the M5Stack NanoC6 to the CN105 connector on the indoor unit's control board using the Grove port:

```
CN105 Connector          M5Stack NanoC6 (Grove)
┌──────────────┐         ┌──────────────────────┐
│ Pin 1 — 12V  │         │                      │
│ Pin 2 — GND ─┼─────────┼─ GND (Black)         │
│ Pin 3 — 5V  ─┼─────────┼─ 5V  (Red)           │
│ Pin 4 — TX  ─┼─────────┼─ GPIO2 / RX (White)  │
│ Pin 5 — RX  ─┼─────────┼─ GPIO1 / TX (Yellow) │
└──────────────┘         └──────────────────────┘
```

> **Note:** The CN105 connector is typically located on the right side of ductless indoor unit's control board behind the front panel. Power is provided by the unit through pins 2 and 3 — no separate power supply is needed. If your board doesn't have a Grove port, you can connect directly to the appropriate GPIO pins for RX/TX, GND, and 5V (see [board profiles](#supported-boards)).

## Setup

### 1. Build and Flash

Clone the repository and flash the firmware:

```bash
git clone https://github.com/akifbayram/mitsubishi-cn105-homekit.git
cd mitsubishi-cn105-homekit

# Build for default board (NanoC6)
pio run

# Or build for a specific board
pio run -e esp32-devkit

# Flash via USB
pio run -t upload --upload-port /dev/ttyACM0
```

### 2. WiFi Provisioning

On first boot, the device creates a WiFi access point:

1. Connect to the **Serin-XXXX** network (XXXX = last 4 hex digits of WiFi MAC, password: `serinlabs`)
2. A captive portal appears — enter your WiFi SSID and password
3. The device saves credentials to flash and reboots
4. A unique 8-digit HomeKit setup code is auto-generated on first boot

<img src="media/recovery.png" width=300>

### 3. WiFi Recovery

If the device loses WiFi connectivity, it automatically enables a fallback AP (**Serin-XXXX**) after 5 minutes, running concurrently with station mode so it continues attempting to reconnect. Three recovery layers are available:

| Layer | Method | Details |
|-------|--------|---------|
| **Auto AP** | Automatic | Fallback AP activates after 5 min disconnect (2 min after a credential change). Disables automatically when WiFi reconnects. |
| **Recovery page** | Web browser | Connect to the AP and navigate to `192.168.4.1:8080` to enter new WiFi credentials. |
| **Button reset** | Physical | 10-second long-press on the board button (e.g., GPIO9 on NanoC6) erases stored WiFi credentials. Only available on boards with a button. |

### 4. HomeKit Pairing

<img src="media/homekit.png" width=300>

Once connected to WiFi:

**Option A — Scan QR Code (recommended):**

1. Open the **Home** app on your iPhone or iPad
2. Tap **+** > **Add Accessory**
3. Scan the QR code shown in the web UI at `http://<device-ip>:8080` (HomeKit panel)

**Option B — Manual setup code:**

1. Open the **Home** app on your iPhone or iPad
2. Tap **+** > **Add Accessory**
3. Select **Mitsubishi Mini Split** (or tap **More options…** if it doesn't appear)
4. Enter the setup code shown in the web UI (HomeKit panel > Setup Code)

## OTA Updates

Update firmware over the air without USB access:

**Via Web UI:**
Navigate to `http://<device-ip>:8080`, open Settings, and use the Firmware Update section. The browser computes a SHA256 checksum before uploading, and the device verifies integrity before applying.

**Via curl:**
```bash
pio run  # build firmware
curl --data-binary @.pio/build/nanoc6/firmware.bin \
     -H "Content-Type: application/octet-stream" \
     http://<device-ip>:8080/upload
```

**Rollback protection:** After an OTA update, the device validates that WiFi and CN105 UART communication are working before confirming the new firmware. If the device reboots before validation (crash, power loss), it automatically rolls back to the previous firmware.

## Web UI

Access the web interface at `http://<device-ip>:8080`.

The single-page interface provides:

- **Mode** — Off/Heat/Cool/Auto/Dry/Fan mode selector (power integrated as Off mode)
- **Temperature** — set target temperature with 0.5°C precision, +/− step buttons
- **Dual Setpoints** — independent heat/cool thresholds in Auto mode (persisted to flash)
- **Fan Speed** — Auto, Quiet, Speed 1–4
- **Vane Control** — vertical and wide vane positions, swing mode
- **Diagnostics** — compressor frequency, outside temp, runtime hours, error codes, sub mode/stage
- **HomeKit** — pairing status, controller count, setup code with copy button, QR code for pairing, reset pairing button
- **Settings** — device name, poll interval (ms), log level, °C/°F toggle
- **Logs** — real-time log streaming via WebSocket
- **OTA** — firmware upload with integrity verification (see [OTA Updates](#ota-updates))

## HomeKit Details

Apple's HomeKit Thermostat service only supports Heat, Cool, Auto, and Off. This section covers the mappings and workarounds for features that don't fit natively.

### Thermostat
| HomeKit Mode | CN105 Mode | Behavior |
|-------------|------------|----------|
| Off | Power Off | Unit off |
| Heat | 0x01 HEAT | Heat to target temperature |
| Cool | 0x03 COOL | Cool to target temperature |
| Auto | 0x08 AUTO | Dual setpoint — heats below heating threshold, cools above cooling threshold |

### FAN & DRY Mode Switches

The Thermostat service has no representation for **FAN** (circulation-only) or **DRY** (dehumidification) modes. These are exposed as separate switches — turning one on powers the unit in that mode, turning it off powers the unit off. Only one mode can be active at a time; switching modes via the thermostat automatically reflects in the switches.

### Fan Speed

HomeKit's Fan service uses a 0–100% rotation speed slider, mapped to discrete speed levels:

| HomeKit % | Speed |
|-----------|-------|
| 0% | Off (powers off unit) |
| 1–20% | Quiet |
| 21–40% | Speed 1 |
| 41–60% | Speed 2 |
| 61–80% | Speed 3 |
| 81–100% | Speed 4 |

Setting the slider to 0% or deactivating the fan service powers the unit off. Auto fan speed is controlled exclusively via the **Fan Auto** switch.

### Dual Setpoint (Auto Mode)

The HomeKit Thermostat service supports independent heating and cooling thresholds, but the CN105 protocol only accepts a **single target temperature**. The controller tracks which side is active via `autoSubMode` from the heat pump's status response and sends the appropriate threshold. A 2°C minimum gap is enforced between thresholds.

### Vane Control

Vane positions (including swing mode) are controlled exclusively through the [web UI](#web-ui).

### Temperature

The heat pump supports 16–31°C. The web UI offers a °C/°F display toggle, but the protocol always operates in Celsius. Half-degree precision is supported when the unit's enhanced temperature encoding is detected.

### Web UI–Only Diagnostics

The following data is available from the heat pump but not exposed as HomeKit services:

| Sensor | Source |
|--------|--------|
| Compressor frequency (Hz) | 0x06 status response |
| Outside air temperature | 0x03 temp response |
| Runtime hours | 0x03 temp response |
| Error code | 0x04 error response |
| Sub mode (Normal/Defrost/Preheat/Standby) | 0x09 standby response |
| Operating stage (Idle/Diffuse) | 0x09 standby response |

## Project Structure

```
src/
  main.cpp                  # Setup, HomeSpan config, accessory tree, LED priority
  cn105_protocol.cpp        # UART driver, packet TX/RX/parsing
  homekit_thermostat.cpp    # HomeKit Thermostat service
  homekit_fan.cpp           # HomeKit Fan + Fan Auto switch services
  homekit_switches.cpp      # HomeKit FAN mode + DRY mode switch services
  web_server.cpp            # HTTP server setup and request routing
  web_ws.cpp                # WebSocket handler, state push, command dispatch
  web_ota.cpp               # OTA firmware upload and verification
  settings.cpp              # NVS persistent settings
  status_led.cpp            # RGB LED state machine and patterns
  wifi_recovery.cpp         # WiFi fallback AP manager, button handler

include/
  cn105_protocol.h          # Protocol constants, state structures
  cn105_strings.h           # Shared enum↔string conversions (log + web + parsers)
  homekit_services.h        # HomeKit service classes
  web_server.h              # Web server interface
  json_utils.h              # Lightweight JSON builder for WebSocket responses
  settings.h                # Settings store
  status_led.h              # LED state enum, StatusLED class
  wifi_recovery.h           # WiFi fallback AP manager
  logging.h                 # Log level macros (conditional HWCDC/Serial)
  board_profile.h           # Board profile selector + defaults
  boards/                   # Per-board hardware definitions
    board_nanoc6.h          #   M5Stack NanoC6 (ESP32-C6)
    board_esp32_devkit.h    #   Generic ESP32 DevKit v1
    board_esp32s3_devkit.h  #   ESP32-S3-DevKitC-1
    board_esp32c3_mini.h    #   ESP32-C3 SuperMini / XIAO
    board_m5atoms3_lite.h   #   M5Stack AtomS3 Lite
  branding.h                # Build-time branding defaults
  web_ui_html.h             # Auto-generated — do not edit
  wifi_recovery_html.h      # Auto-generated — do not edit

web/
  index.html                # Web UI source (edit this)
  recovery.html             # WiFi recovery page source (edit this)

scripts/
  embed_html.py             # Gzips HTML → generates web_ui_html.h
  pre_build.py              # PlatformIO pre-build: auto-runs embed_html.py
  provision.sh              # Per-unit provisioning: flash, read MAC, generate QR label

partitions.csv                # Custom partition table (dual OTA, no SPIFFS)
```

## CN105 Protocol

The CN105 connector uses a serial protocol at 2400 baud with 8E1 (even parity). For detailed protocol documentation, see the [muart-group wiki](https://muart-group.github.io/).

| Byte | Field |
|------|-------|
| 0 | `0xFC` sync byte |
| 1 | Packet type |
| 2–3 | `0x01 0x30` header |
| 4 | Data length |
| 5+ | Data payload |
| Last | Checksum: `(0xFC - sum_of_all_bytes) & 0xFF` |

**Polling cycle** (5 phases, default 2s interval):

| Phase | Type | Data |
|-------|------|------|
| 0x02 | Settings | Power, mode, target temp, fan, vane, wide vane |
| 0x03 | Room temp | Room temperature, outside temperature, runtime hours |
| 0x04 | Error | Error code (0x80 = normal) |
| 0x06 | Status | Operating state, compressor frequency |
| 0x09 | Standby | Sub mode, stage, auto sub mode |

## Acknowledgments

This project builds on the work of several open-source projects in the Mitsubishi heat pump community:

- **[HomeSpan](https://github.com/HomeSpan/HomeSpan)** — ESP32 HomeKit library
- **[SwiCago/HeatPump](https://github.com/SwiCago/HeatPump)** — the original CN105 protocol library and compatibility documentation
- **[esphome-mitsubishiheatpump](https://github.com/geoffdavis/esphome-mitsubishiheatpump)** — early ESPHome integration
- **[MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome)** — ESPHome component with comprehensive CN105 protocol implementation
- **[muart-group](https://muart-group.github.io/)** — community documentation and protocol research

## Trademarks

Apple, Apple Home, and HomeKit are trademarks of Apple Inc. Mitsubishi Electric is a trademark of Mitsubishi Electric Corporation. This project is not certified by, endorsed by, or affiliated with Apple Inc. or Mitsubishi Electric Corporation.

## License

MIT