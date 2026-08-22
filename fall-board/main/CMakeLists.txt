idf_component_register(
    SRCS "main.cpp"
         "radar.cpp"
         "network.cpp"
         "ota.cpp"
         "telemetry.cpp"
    INCLUDE_DIRS "."
                 "model"
    REQUIRES esp-tflite-micro esp32-camera esp_psram esp-nn esp_timer driver esp_pm
             esp_wifi esp_event esp_netif nvs_flash esp_http_client
             esp_https_ota app_update mqtt
             json led_strip
)
