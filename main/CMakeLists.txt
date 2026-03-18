idf_component_register(
    SRCS "main.cpp"
         "logging.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server esp_timer driver bt
)
