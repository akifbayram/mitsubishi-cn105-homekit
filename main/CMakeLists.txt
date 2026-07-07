idf_component_register(
    SRCS "main.cpp"
         "logging.cpp"
         "settings.cpp"
         "status_led.cpp"
         "cn105_protocol.cpp"
         "wifi_manager.cpp"
         "dns_server.cpp"
         "wifi_recovery.cpp"
         "improv_serial.cpp"
         "web_server.cpp"
         "web_ws.cpp"
         "web_ota.cpp"
         "homekit_setup.cpp"
         "homekit_services_glue.cpp"
         "homekit_thermostat.cpp"
         "homekit_fan.cpp"
         "homekit_switches.cpp"
         "ble_sensor.cpp"
         "homekit_sensor_accessory.cpp"
         "espnow_bond.cpp"
         "espnow_link.cpp"
         "espnow_crypto.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer driver bt app_update mbedtls
             esp_hap_core esp_hap_apple_profiles esp_hap_platform espressif__mdns
             console
)

# ── Pass firmware version from git to source code ────────────────────────────
target_compile_definitions(${COMPONENT_TARGET} PRIVATE FW_VERSION="${PROJECT_VER}")

# ── Auto-select board profile based on IDF_TARGET ────────────────────────────
# Each chip gets a default board profile. Override by setting -DBOARD_PROFILE_*
# on the cmake command line (e.g., idf.py build -DBOARD_PROFILE_M5ATOMS3_LITE=1).
# When adding a board here, also update the BOARD_FW_ID chain below.
set(_explicit_profile_set FALSE)
foreach(_prof CUSTOM NANOC6 ESP32S3_DEVKIT ESP32_DEVKIT ESP32C3_MINI M5ATOMS3_LITE)
    if(DEFINED BOARD_PROFILE_${_prof})
        target_compile_definitions(${COMPONENT_TARGET} PRIVATE BOARD_PROFILE_${_prof})
        set(_explicit_profile_set TRUE)
    endif()
endforeach()

if(NOT _explicit_profile_set)
    # No override — auto-select by chip
    if(CONFIG_IDF_TARGET_ESP32C6)
        target_compile_definitions(${COMPONENT_TARGET} PRIVATE BOARD_PROFILE_NANOC6)
    elseif(CONFIG_IDF_TARGET_ESP32S3)
        target_compile_definitions(${COMPONENT_TARGET} PRIVATE BOARD_PROFILE_ESP32S3_DEVKIT)
    elseif(CONFIG_IDF_TARGET_ESP32)
        target_compile_definitions(${COMPONENT_TARGET} PRIVATE BOARD_PROFILE_ESP32_DEVKIT)
    elseif(CONFIG_IDF_TARGET_ESP32C3)
        target_compile_definitions(${COMPONENT_TARGET} PRIVATE BOARD_PROFILE_ESP32C3_MINI)
    endif()
endif()

# ── Update check (browser-mediated OTA via serin-cn105 manifest) ─────────────
# BOARD_FW_ID maps the board profile to its serin-cn105 firmware directory.
# Empty = no published prebuilt (CUSTOM, ESP32S3_DEVKIT) → update UI hidden.
# -DUPDATE_MANIFEST_URL="" disables the feature; branded builds may point elsewhere.
if(NOT DEFINED UPDATE_MANIFEST_URL)
    set(UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/Serin-Labs/serin-cn105/main/firmware/homekit/manifest.json")
endif()

set(BOARD_FW_ID "")
if(DEFINED BOARD_PROFILE_NANOC6)
    set(BOARD_FW_ID "nanoc6")
elseif(DEFINED BOARD_PROFILE_M5ATOMS3_LITE)
    set(BOARD_FW_ID "m5atoms3-lite")
elseif(DEFINED BOARD_PROFILE_ESP32_DEVKIT)
    set(BOARD_FW_ID "esp32-devkit")
elseif(DEFINED BOARD_PROFILE_ESP32C3_MINI)
    set(BOARD_FW_ID "esp32c3-mini")
elseif(NOT _explicit_profile_set)
    # Auto-selected profiles (mirror the chip → profile mapping above).
    # esp32s3 auto-selects ESP32S3_DEVKIT, which has no published prebuilt.
    if(CONFIG_IDF_TARGET_ESP32C6)
        set(BOARD_FW_ID "nanoc6")
    elseif(CONFIG_IDF_TARGET_ESP32)
        set(BOARD_FW_ID "esp32-devkit")
    elseif(CONFIG_IDF_TARGET_ESP32C3)
        set(BOARD_FW_ID "esp32c3-mini")
    endif()
endif()

# ── BLE remote sensor build-time defaults ────────────────────────────────────
# Provision the remote temperature sensor at flash time so no post-flash setup
# is needed. These seed first-boot defaults only; values changed later via the
# web UI are stored in NVS and win on subsequent boots.
#   -DBLE_SENSOR_ADDR="AA:BB:CC:DD:EE:FF"  default sensor MAC
#   -DBLE_SENSOR_DEFAULT_ON=1              enable the BLE sensor by default
#   -DBLE_DISABLE=1                        compile BLE support out entirely
if(DEFINED BLE_SENSOR_ADDR)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE BLE_SENSOR_ADDR="${BLE_SENSOR_ADDR}")
endif()
if(BLE_SENSOR_DEFAULT_ON)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE BLE_SENSOR_DEFAULT_ON=1)
endif()
if(BLE_DISABLE)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE BLE_DISABLE=1)
endif()

# ── Brand defaults (overridable via -DBRAND_NAME=... on cmake command line) ──
if(NOT DEFINED BRAND_NAME)
    set(BRAND_NAME "Mini Split")
endif()
if(NOT DEFINED BRAND_AP_PREFIX)
    set(BRAND_AP_PREFIX "Serin")
endif()
if(NOT DEFINED BRAND_AP_PASSWORD)
    set(BRAND_AP_PASSWORD "serinlabs")
endif()
if(NOT DEFINED BRAND_MODEL)
    set(BRAND_MODEL "CN105")
endif()
if(NOT DEFINED BRAND_THEME_COLOR)
    set(BRAND_THEME_COLOR "#1a73e8")
endif()

# ── Gzip + template substitute HTML files ────────────────────────────────────
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web_ui.html.gz
    COMMAND python3 ${PROJECT_DIR}/scripts/embed_html_idf.py
        --input ${PROJECT_DIR}/web/index.html
        --output ${CMAKE_CURRENT_BINARY_DIR}/web_ui.html.gz
        --brand-name "${BRAND_NAME}"
        --brand-ap-prefix "${BRAND_AP_PREFIX}"
        --brand-ap-password "${BRAND_AP_PASSWORD}"
        --brand-model "${BRAND_MODEL}"
        --brand-theme-color "${BRAND_THEME_COLOR}"
        --fw-version "${PROJECT_VER}"
        --board-fw-id "${BOARD_FW_ID}"
        --update-manifest-url "${UPDATE_MANIFEST_URL}"
    DEPENDS ${PROJECT_DIR}/web/index.html ${PROJECT_DIR}/scripts/embed_html_idf.py
    COMMENT "Generating web_ui.html.gz"
    VERBATIM
)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/recovery.html.gz
    COMMAND python3 ${PROJECT_DIR}/scripts/embed_html_idf.py
        --input ${PROJECT_DIR}/web/recovery.html
        --output ${CMAKE_CURRENT_BINARY_DIR}/recovery.html.gz
        --brand-name "${BRAND_NAME}"
        --brand-ap-prefix "${BRAND_AP_PREFIX}"
        --brand-ap-password "${BRAND_AP_PASSWORD}"
    DEPENDS ${PROJECT_DIR}/web/recovery.html ${PROJECT_DIR}/scripts/embed_html_idf.py
    COMMENT "Generating recovery.html.gz"
    VERBATIM
)

# ── Embed all binary assets via linker symbols ───────────────────────────────
target_add_binary_data(${COMPONENT_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/web_ui.html.gz BINARY)
target_add_binary_data(${COMPONENT_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/recovery.html.gz BINARY)
target_add_binary_data(${COMPONENT_TARGET} ${PROJECT_DIR}/web/icon-192.png BINARY)
target_add_binary_data(${COMPONENT_TARGET} ${PROJECT_DIR}/web/icon-512.png BINARY)
target_add_binary_data(${COMPONENT_TARGET} ${PROJECT_DIR}/web/favicon.png BINARY)
