idf_component_register(
    SRCS "main.cpp"
         "logging.cpp"
         "settings.cpp"
         "status_led.cpp"
         "cn105_protocol.cpp"
         "wifi_manager.cpp"
         "dns_server.cpp"
         "wifi_recovery.cpp"
         "web_server.cpp"
         "web_ws.cpp"
         "web_ota.cpp"
         "homekit_setup.cpp"
         "homekit_services_glue.cpp"
         "homekit_thermostat.cpp"
         "homekit_fan.cpp"
         "homekit_switches.cpp"
         "ble_sensor.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer driver bt app_update mbedtls
             esp_hap_core esp_hap_apple_profiles esp_hap_platform espressif__mdns
)

# ── Auto-select board profile based on IDF_TARGET ────────────────────────────
# Each chip gets a default board profile. Override by setting -DBOARD_PROFILE_*
# on the cmake command line (e.g., idf.py build -DBOARD_PROFILE_M5ATOMS3_LITE=1).
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
