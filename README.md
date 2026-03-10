# Mitsubishi CN105 HomeKit Controller

Controls Mitsubishi mini split heat pumps via the CN105 serial connector, compatible with Apple Home through the HomeKit Accessory Protocol (HAP). No cloud, no bridge, no Home Assistant required.

Built for the [M5Stack NanoC6](https://shop.m5stack.com/products/m5stack-nanoc6-dev-kit) (ESP32-C6).

> [!CAUTION]
> **Use at your own risk.** This is an unofficial, community-driven implementation based on the reverse-engineered CN105 serial protocol. It is not developed, endorsed, or supported by Mitsubishi Electric or Apple. Connecting third-party hardware to your heat pump may void its warranty. Not all units support every feature, and behavior may vary by model. The authors and contributors provide this software as-is, with no warranty or guarantee of any kind. 

## Features

- **HomeKit** — Heat, Cool, Auto, Off, FAN mode, DRY mode, fan speed, dual setpoint, half-degree precision
- **[Web UI](#web-ui)** — full thermostat controls, diagnostics, vane control, HomeKit pairing, settings, OTA updates, live logs (port 8080)
- **Status LED** — RGB LED for boot sequence, WiFi/CN105 status, error codes, OTA indication
- **[CN105 Protocol](#cn105-protocol)** — direct serial at 2400 baud 8E1, 5-phase polling, anti-flicker pattern

## Requirements

### Hardware

| Component | Details |
|-----------|---------|
| **Microcontroller** | [M5Stack NanoC6](https://shop.m5stack.com/products/m5stack-nanoc6-dev-kit) (ESP32-C6) |
| **Connector** | Grove (HY2.0-4P) to CN105 cable |
| **Heat pump** | Mitsubishi mini split with CN105 connector |

Most Mitsubishi ductless and ducted units manufactured after 2010 have a CN105 connector on the indoor unit's control board. 

### Software

- [PlatformIO](https://platformio.org/) (build system)
- Python 3 (for HTML embedding script)

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

> **Note:** The CN105 connector is typically located on the right side of ductless indoor unit's control board behind the front panel. Power is provided by the unit through pins 2 and 3 — no separate power supply is needed.

## Setup

### 1. Build and Flash

Clone the repository and flash the firmware:

```bash
git clone https://github.com/akifbayram/mitsubishi-cn105-homekit.git
cd mitsubishi-cn105-homekit

# Build (web UI HTML is embedded automatically via pre-build script)
pio run

# Flash via USB
pio run -t upload --upload-port /dev/ttyACM0
```

### 2. WiFi Provisioning

On first boot, the device creates a WiFi access point:

1. Connect to the **Serin-XXXX** network (XXXX = last 4 hex digits of WiFi MAC, password: `serinlabs`)
2. A captive portal appears — enter your WiFi SSID and password
3. The device saves credentials to flash and reboots
4. A unique 8-digit HomeKit setup code is auto-generated on first boot

**WiFi Recovery:** If the device loses WiFi connectivity, it automatically enables a fallback AP (Serin-XXXX) after 5 minutes, running concurrently with station mode. Connect to reconfigure credentials. The AP disables automatically when WiFi reconnects. GPIO9 long-press (10s) erases stored credentials.

### 3. HomeKit Pairing

Once connected to WiFi:

1. Open the **Home** app on your iPhone or iPad
2. Tap **+** > **Add Accessory**
3. Select **Mitsubishi Mini Split**
4. Enter the setup code you chose during provisioning

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
  homekit_services.cpp      # Thermostat, Fan, FanAutoSwitch, FAN/DRY switches
  web_server.cpp            # HTTP server, WebSocket, OTA upload
  settings.cpp              # NVS persistent settings
  status_led.cpp            # RGB LED state machine and patterns
  wifi_recovery.cpp         # WiFi fallback AP manager, GPIO9 button handler

include/
  cn105_protocol.h          # Protocol constants, state structures
  homekit_services.h        # HomeKit service classes
  web_server.h              # Web server interface
  settings.h                # Settings store
  status_led.h              # LED state enum, StatusLED class
  wifi_recovery.h           # WiFi fallback AP manager
  logging.h                 # Log level macros
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

The CN105 connector uses a serial protocol at 2400 baud with 8E1 (even parity):

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