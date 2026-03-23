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

## Features

- **Native Apple HomeKit** — no bridge, cloud, or Home Assistant required
- **Web UI** — real-time control, diagnostics, and log streaming
- **Browser-based flashing** — no development tools needed ([web flasher](https://serin-labs.github.io/flash))
- **BLE remote temperature sensor** — Govee, Xiaomi (PVVX), BTHome v2, with auto-detection
- **OTA firmware updates** — with SHA256 verification and automatic rollback
- **Dual setpoint Auto mode** — independent heating and cooling thresholds
- **Multi-board support** — ESP32, ESP32-C3, ESP32-C6, ESP32-S3
- **WiFi recovery** — automatic fallback AP with web-based credential entry

## Quick Start

1. **Flash** — Open the [web flasher](https://serin-labs.github.io/flash), connect your board via USB, and flash the firmware from your browser
2. **Connect** — Join the **Serin-XXXX** WiFi network (password: `serinlabs`) and enter your WiFi credentials
3. **Pair** — Open Apple Home, scan the QR code from the web UI at `http://<device-ip>:8080`

For developer setup with ESP-IDF, custom boards, and build-time options, see [Setup](#setup).

## Requirements

### Hardware

<img src="media/wiring.png" width=300>

| Component | Details |
|-----------|---------|
| **Microcontroller** | Any [supported board](#boards) (default: M5Stack NanoC6) |
| **Connector** | Grove (HY2.0-4P) to CN105 cable (NanoC6) |
| **Heat pump** | Mitsubishi mini split with CN105 connector |

For a full parts list with purchase links, see the [parts guide](https://serin-labs.github.io/parts). For compatible models, see the [compatibility list](https://serin-labs.github.io/compatibility) or the [MitsubishiCN105ESPHome supported units list](https://github.com/echavet/MitsubishiCN105ESPHome?tab=readme-ov-file#supported-mitsubishi-climate-units). Not every unit supports every feature (outside temperature, half-degree precision, wide vane control); it varies by model.

### Software

- [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/) (build framework)
- Python 3 (for HTML embedding script)

### Boards

| Board | Target | Build command | Tested |
|-------|--------|---------------|-------|
| M5Stack NanoC6 (ESP32-C6) | `esp32c6` | `idf.py set-target esp32c6 && idf.py build` | ✅ |
| M5Stack AtomS3 Lite (ESP32-S3) | `esp32s3` | `idf.py set-target esp32s3 && idf.py build` | ✅ |
| ESP32-C3 SuperMini / XIAO | `esp32c3` | `idf.py set-target esp32c3 && idf.py build` | ❌ |
| Generic ESP32 DevKit | `esp32` | `idf.py set-target esp32 && idf.py build` | ❌ |

Board profiles define GPIO pins, LED, button, UART clock source, and debug output for each target. See `main/boards/` for details, or the [Custom Board Guide](docs/custom-boards.md) to add a new board.

## Wiring

Connect the M5Stack NanoC6 to the CN105 connector on the indoor unit's control board using the Grove port. For detailed wiring photos and diagrams, see the [wiring guide](https://serin-labs.github.io/wiring).

```
CN105 Connector          M5Stack NanoC6 (Grove)
┌──────────────┐
│ Pin 1 — 12V  │         ┌──────────────────────┐
│ Pin 2 — GND ─┼─────────┼─ GND (Black)         │
│ Pin 3 — 5V  ─┼─────────┼─ 5V  (Red)           │
│ Pin 4 — TX  ─┼─────────┼─ GPIO2 / RX (White)  │
│ Pin 5 — RX  ─┼─────────┼─ GPIO1 / TX (Yellow) │
└──────────────┘         └──────────────────────┘
```

The CN105 connector is typically located on the right side of ductless indoor unit's control board behind the front panel. Power is provided by the unit through pins 2 and 3. If your board doesn't have a Grove port, you can connect directly to the appropriate GPIO pins for RX/TX, GND, and 5V (see [board profiles](#boards)).

## Setup

### 1. Flash Firmware

**Option A — Web Flasher (recommended):**

No tools to install. Visit the [web flasher](https://serin-labs.github.io/flash), connect your board via USB, select your board type, and click flash. Works in Chrome and Edge.

**Option B — ESP-IDF (developers):**

```bash
git clone --recursive https://github.com/akifbayram/mitsubishi-cn105-homekit.git
cd mitsubishi-cn105-homekit

# Activate ESP-IDF environment
source ~/esp/esp-idf/export.sh

# Build for default board (NanoC6 / ESP32-C6)
idf.py set-target esp32c6
idf.py build

# Flash via USB
idf.py -p /dev/ttyACM0 flash

# Monitor serial output
idf.py -p /dev/ttyACM0 monitor
```

### 2. WiFi Provisioning

On first boot, the device creates a WiFi access point:

1. Connect to the **Serin-XXXX** network (XXXX = last 4 hex digits of WiFi MAC, password: `serinlabs`)
2. A captive portal appears. Enter your WiFi SSID and password
3. The device saves credentials to flash and reboots
4. A unique 8-digit HomeKit setup code is auto-generated on first boot

<img src="media/recovery.png" width=300>

**Build-time WiFi (optional):** To skip the captive portal during development, pass WiFi credentials as CMake flags:

```bash
idf.py -DWIFI_SSID="MyNetwork" -DWIFI_PASSWORD="MyPassword" build
```

The device will connect automatically on boot. WiFi can still be changed later via the web UI.

### 3. WiFi Recovery

If the device loses WiFi, it spins up a fallback AP (**Serin-XXXX**) after 5 minutes while still trying to reconnect in the background. Three recovery options:

| Layer | Method | Details |
|-------|--------|---------|
| **Auto AP** | Automatic | Fallback AP activates after 5 min disconnect (2 min after a credential change). Disables automatically when WiFi reconnects. |
| **Recovery page** | Web browser | Connect to the AP and navigate to `192.168.4.1:8080` to enter new WiFi credentials. |
| **Button reset** | Physical | 10-second long-press on the board button (e.g., GPIO9 on NanoC6) erases stored WiFi credentials. Only available on boards with a button. |

### 4. HomeKit Pairing

<img src="media/homekit.png" width=300>

Once connected to WiFi:

**Option A — Scan QR Code (recommended):**

1. Scan the QR code shown in the web UI at `http://<device-ip>:8080` (HomeKit panel)

**Option B — Manual setup code:**

1. Open the **Home** app on your iPhone or iPad
2. Tap **+** > **Add Accessory**
3. Select **Mini Split XXXX** (or tap **More options…** if it doesn't appear)
4. Enter the setup code shown in the web UI (HomeKit panel > Setup Code)

### 5. Status LEDs

On boards with an RGB LED:

| Color | Meaning |
|-------|---------|
| White blink | Booting |
| Red steady | CN105 disconnected |
| Red fast blink | Error |
| Blue pulse | OTA in progress |
| Off | Normal operation |

On the NanoC6, a separate blue LED tracks WiFi (on = disconnected, off = connected). Boards without a blue LED show WiFi disconnect as steady blue on the RGB LED instead (vs pulsing blue for OTA).

## Remote Temperature Sensor (BLE)

Wall-mounted units measure temperature at ceiling height near the return air intake, which usually reads warmer than the living space. A BLE sensor placed lower in the room gives the heat pump a better reading to work with.

The firmware listens for BLE advertisements from a configured sensor. No Bluetooth pairing needed, just power on the sensor. Temperature is forwarded to the heat pump every 20 seconds. If no BLE data arrives for 90 seconds, the heat pump reverts to its internal thermistor.

### Supported Sensors

The firmware auto-detects the sensor type from the BLE advertisement format.

| Protocol | Devices | Tested |
|----------|---------|:------:|
| Govee V3 | H5072, H5075 | ❌ |
| Govee V2 | H5074, H5051, H5052, H5071 | ✅ |
| Govee V1 | H5100, H5101, H5102, H5103, H5104, H5105, H5108, H5110, H5174, H5177, GV5179 | ✅ |
| PVVX | Xiaomi LYWSD03MMC, CGG1 (requires [PVVX custom firmware](https://github.com/pvvx/ATC_MiThermometer)) | ❌ |
| BTHome v2 | SwitchBot, Shelly, or any BTHome v2 device | ❌ |

### Web UI Configuration

A **Remote Sensor** card appears in the web UI on boards with Bluetooth:

- **MAC Address** — enter or change the sensor's BLE MAC address (persisted to flash)
- **Feed Toggle** — enable/disable sending the sensor temperature to the heat pump (disabling the feed keeps scanning and displaying sensor data, it just stops overriding the HP's internal thermistor)
- **Status** — live temperature, humidity, battery level, signal strength, and last update time
- **Indicators** — green (active), orange (feed disabled), red (stale data), gray (scanning/not configured)

## OTA Updates

Update firmware over the air without USB access:

**Via Web UI:**
Navigate to `http://<device-ip>:8080`, open Settings, and use the Firmware Update section. The browser computes a SHA256 checksum before uploading, and the device verifies integrity before applying.

**Via curl:**
```bash
idf.py build
curl --data-binary @build/mitsubishi-cn105-homekit.bin \
     -H "Content-Type: application/octet-stream" \
     http://<device-ip>:8080/upload
```

**Rollback protection:** After an OTA update, the device checks that WiFi and CN105 UART still work before confirming the new firmware. If it reboots before that check passes (crash, power loss), it rolls back to the previous firmware.

## Web UI

Access the web interface at `http://<device-ip>:8080`.

The web UI includes:

- **Mode** — Off/Heat/Cool/Auto/Dry/Fan mode selector (power integrated as Off mode)
- **Temperature** — set target temperature with 0.5°C precision, +/− step buttons
- **Dual Setpoints** — independent heat/cool thresholds in Auto mode (persisted to flash)
- **Fan Speed** — Auto, Quiet, Speed 1–4
- **Vane Control** — vertical and wide vane positions, swing mode
- **Remote Sensor** — BLE sensor temperature, humidity, battery, signal strength, MAC config, feed toggle (only visible when built with BLE support)
- **Diagnostics** — compressor frequency, outside temp, runtime hours, error codes, sub mode/stage
- **HomeKit** — pairing status, controller count, setup code with copy button, QR code for pairing, reset pairing button
- **Settings** — device name, poll interval (ms), log level, °C/°F toggle
- **Logs** — real-time log streaming via WebSocket
- **OTA** — firmware upload with integrity verification (see [OTA Updates](#ota-updates))

## HomeKit Details

Thermostat mode mappings, FAN/DRY mode switches, fan speed percentages, dual setpoints, vane control, and diagnostics are documented in [HomeKit Details](docs/homekit.md). For an overview of HomeKit features and setup, see the [Serin Labs HomeKit page](https://serin-labs.github.io/homekit/features).

## Project Structure

See [Project Structure](docs/project-structure.md) for the full source tree with descriptions of each file.

## CN105 Protocol

2400 baud, 8E1 serial protocol over the CN105 connector. See [Protocol Reference](docs/protocol.md) for packet format and polling cycle details, and the [muart-group wiki](https://muart-group.github.io/) for community protocol documentation.

## Troubleshooting

See the [troubleshooting guide](https://serin-labs.github.io/troubleshooting) for help with flashing, WiFi, HomeKit pairing, and CN105 connection issues.

## Acknowledgments

Built on work from:

- **[esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk)** — Espressif's official HomeKit SDK for ESP-IDF
- **[SwiCago/HeatPump](https://github.com/SwiCago/HeatPump)** — the original CN105 protocol library and compatibility documentation
- **[esphome-mitsubishiheatpump](https://github.com/geoffdavis/esphome-mitsubishiheatpump)** — early ESPHome integration
- **[MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome)** — ESPHome component with comprehensive CN105 protocol implementation
- **[muart-group](https://muart-group.github.io/)** — community documentation and protocol research

## Trademarks

Apple, Apple Home, and HomeKit are trademarks of Apple Inc. Mitsubishi Electric is a trademark of Mitsubishi Electric Corporation. This project is not certified by, endorsed by, or affiliated with Apple Inc. or Mitsubishi Electric Corporation.

## License

MIT