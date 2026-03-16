# Project Structure

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
  ble_sensor.cpp            # BLE scanner, sensor decoders, keepalive/stale logic

include/
  cn105_protocol.h          # Protocol constants, state structures
  cn105_strings.h           # Shared enum-string conversions (log + web + parsers)
  homekit_services.h        # HomeKit service classes
  web_server.h              # Web server interface
  json_utils.h              # Lightweight JSON builder for WebSocket responses
  settings.h                # Settings store
  status_led.h              # LED state enum, StatusLED class
  wifi_recovery.h           # WiFi fallback AP manager
  ble_sensor.h              # BLE sensor types, public API, compile guards
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
  embed_html.py             # Gzips HTML -> generates web_ui_html.h
  pre_build.py              # PlatformIO pre-build: auto-runs embed_html.py
  provision.sh              # Per-unit provisioning: flash, read MAC, generate QR label

partitions.csv                # Custom partition table (dual OTA, no SPIFFS)
```
