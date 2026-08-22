// MQTT 遥测：每 10 秒上报板子运行数据到 fall/telemetry/<device_id>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "telemetry.h"
#include "ota.h"
#include "device_config.h"

static const char *TAG = "telemetry";
static esp_mqtt_client_handle_t mqtt = NULL;
static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static volatile bool mqtt_connected = false;

static void telemetry_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(20000)); // 等 WiFi/网络栈就绪
    if (mqtt == NULL) {
        esp_mqtt_client_config_t cfg = {};
        cfg.broker.address.uri = "mqtt://" MQTT_HOST;
        cfg.broker.address.port = MQTT_PORT;
        cfg.credentials.username = MQTT_USER;
        cfg.credentials.authentication.password = MQTT_PASS;
        mqtt = esp_mqtt_client_init(&cfg);
        esp_mqtt_client_register_event(mqtt, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event, NULL);
        esp_mqtt_client_start(mqtt);
    }
    vTaskDelay(pdMS_TO_TICKS(15000)); // 等 MQTT 连接
    for (;;) {
        if (mqtt && mqtt_connected) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "fw", FW_VERSION);
            cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
            cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                cJSON_AddNumberToObject(root, "rssi", ap.rssi);
            }
            cJSON_AddNumberToObject(root, "restart_reason", esp_reset_reason());
            char *s = cJSON_PrintUnformatted(root);
            if (s) {
                esp_mqtt_client_publish(mqtt, TELEMETRY_TOPIC, s, 0, 0, 0);
                free(s);
            }
            cJSON_Delete(root);
        }
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_S * 1000));
    }
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT 已连接，遥测主题 %s", TELEMETRY_TOPIC);
        char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "fall/commands/%s", DEVICE_ID);
        int sub = esp_mqtt_client_subscribe(mqtt, cmd_topic, 0);
        ESP_LOGI(TAG, "订阅命令主题 %s (rc=%d)", cmd_topic, sub);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT 断开，重连中...");
        break;
    case MQTT_EVENT_DATA: {
        if (ev && ev->topic_len && ev->data_len) {
            char topic[128] = {0};
            char data[64] = {0};
            int tl = ev->topic_len < 127 ? ev->topic_len : 127;
            int dl = ev->data_len < 63 ? ev->data_len : 63;
            memcpy(topic, ev->topic, tl);
            memcpy(data, ev->data, dl);
            ESP_LOGI(TAG, "收到命令 %s: %s", topic, data);
            if (strncmp(data, "ota_check", 9) == 0) {
                ESP_LOGI(TAG, "触发 OTA 检查");
                ota_check_now();
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 错误");
        break;
    default:
        break;
    }
}

void telemetry_init(void) {
    // 网络栈就绪后才启动 MQTT（app_main 开头调用时 WiFi 尚未初始化，立即 start 会触发 lwip assert）
    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 4, NULL);
}
