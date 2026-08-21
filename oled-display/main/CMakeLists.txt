idf_component_register(
    SRCS "main.c" "oled.c" "network.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif
             json driver mqtt esp_timer
)
