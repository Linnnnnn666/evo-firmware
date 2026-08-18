idf_component_register(
    SRCS "main.cpp" "network.cpp" "ota.cpp" "telemetry.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client
             esp_https_ota app_update mqtt esp_timer driver json
)
