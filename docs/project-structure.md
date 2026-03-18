# Project Structure

```
main/
  main.cpp                    # app_main(), init sequence, FreeRTOS main loop
  cn105_protocol.cpp/h        # UART driver, packet TX/RX/parsing, WantedSettings
  cn105_strings.h             # Shared enum-string conversions (log + web + parsers)
  homekit_setup.cpp/h         # HAP init, setup code gen, event handler, status
  homekit_services.h          # Service creation + sync interface
  homekit_services_glue.cpp   # Service dispatcher, CN105 controller binding
  homekit_thermostat.cpp      # Thermostat service (write callback, sync, dual setpoint)
  homekit_fan.cpp             # Fan v2 + Fan Auto switch services
  homekit_switches.cpp        # FAN mode + DRY mode switch services
  web_server.cpp/h            # HTTP server setup, request routing, HomeKit status
  web_ws.cpp                  # WebSocket handler, state push, command dispatch
  web_ota.cpp                 # OTA firmware upload with SHA256 verification
  settings.cpp/h              # NVS persistent settings (direct nvs_get/set API)
  logging.cpp/h               # ESP_LOG* macros, custom vprintf handler, WS log hook
  status_led.cpp/h            # RGB LED state machine via led_strip RMT driver
  wifi_manager.cpp/h          # esp_wifi + esp_netif, event-driven, NVS credentials
  wifi_recovery.cpp/h         # 3-layer WiFi recovery (AP fallback, recovery page, button)
  dns_server.cpp/h            # Lightweight UDP DNS captive portal for recovery AP
  ble_sensor.cpp/h            # Native NimBLE scanner, auto-detect decoders, keepalive
  ble_config.h                # BLE_ENABLE auto-define (standalone, no dependencies)
  board_profile.h             # Board profile selector + defaults
  boards/                     # Per-board hardware definitions
    board_nanoc6.h            #   M5Stack NanoC6 (ESP32-C6)
    board_esp32_devkit.h      #   Generic ESP32 DevKit v1
    board_esp32s3_devkit.h    #   ESP32-S3-DevKitC-1
    board_esp32c3_mini.h      #   ESP32-C3 SuperMini / XIAO
    board_m5atoms3_lite.h     #   M5Stack AtomS3 Lite
  branding.h                  # Build-time branding defaults
  compat_arduino.h            # millis(), delay(), constrain() shims
  uart_interface.h            # UART abstraction interface
  hardware_uart.h             # ESP-IDF UART driver implementation
  json_utils.h                # Lightweight JSON string parser

components/
  esp-homekit-sdk/            # Espressif HomeKit SDK (git submodule)

web/
  index.html                  # Web UI source (edit this; CMake auto-embeds gzipped)
  recovery.html               # WiFi recovery page source
  icon-192.png                # PWA icon 192x192
  icon-512.png                # PWA icon 512x512

scripts/
  embed_html_idf.py           # Gzips HTML with {{BRAND_*}} template substitution
  provision.sh                # Per-unit provisioning: flash, read MAC, generate QR label

partitions.csv                # 8MB flash partition table (dual OTA)
partitions_4mb.csv            # 4MB flash partition table (NanoC6)
sdkconfig.defaults            # Shared ESP-IDF config
sdkconfig.defaults.esp32c6    # ESP32-C6 overrides (4MB flash, USB JTAG)
sdkconfig.defaults.esp32s3    # ESP32-S3 overrides (USB JTAG)
sdkconfig.defaults.esp32      # ESP32 classic overrides (BLE disabled)
sdkconfig.defaults.esp32c3    # ESP32-C3 overrides
CMakeLists.txt                # Top-level ESP-IDF project + component paths
```
