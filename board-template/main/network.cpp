// 轻量网络层：nvs + esp_netif + WiFi STA 连接（供 telemetry/ota 使用）
#include "network.h"
#include "board_config.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "net";

static EventGroupHandle_t g_ev = nullptr;
static bool g_started = false;

#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_ev) xEventGroupClearBits(g_ev, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *got_ip = static_cast<const ip_event_got_ip_t *>(event_data);
        if (g_ev) xEventGroupSetBits(g_ev, WIFI_CONNECTED_BIT);
        printf("[WIFI] connected, IP=" IPSTR "\n", IP2STR(&got_ip->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

bool network_init(void) {
    if (g_started) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
        return false;
    }
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        ESP_LOGE(TAG, "create wifi sta failed");
        return false;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (g_ev == nullptr) {
        g_ev = xEventGroupCreate();
        if (g_ev == nullptr) return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, nullptr));

    wifi_config_t sta = {};
    strlcpy(reinterpret_cast<char *>(sta.sta.ssid), BOARD_WIFI_SSID,
            sizeof(sta.sta.ssid));
    strlcpy(reinterpret_cast<char *>(sta.sta.password), BOARD_WIFI_PASSWORD,
            sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    g_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started, SSID=%s", BOARD_WIFI_SSID);
    return true;
}

bool network_wait_connected(uint32_t timeout_ms) {
    if (g_ev == nullptr) return false;
    return xEventGroupWaitBits(g_ev, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) != 0;
}
