idf_component_register(
    SRCS "main.c" "network.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client
             json driver esp-serial-flasher mqtt esp_timer
)
