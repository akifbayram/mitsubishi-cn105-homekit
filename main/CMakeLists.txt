idf_component_register(
    SRCS "main.cpp"
         "logging.cpp"
         "settings.cpp"
         "status_led.cpp"
         "cn105_protocol.cpp"
         "wifi_manager.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer driver bt
)

# ── Brand defaults (overridable via -DBRAND_NAME=... on cmake command line) ──
if(NOT DEFINED BRAND_NAME)
    set(BRAND_NAME "Serin")
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
