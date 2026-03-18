idf_component_register(
    SRCS "main.cpp"
         "logging.cpp"
         "settings.cpp"
         "status_led.cpp"
         "cn105_protocol.cpp"
    INCLUDE_DIRS "." "../include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer driver bt
)
