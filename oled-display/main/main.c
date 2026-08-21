// OLED 显示板：MQTT 订阅烧录事件 → SSD1306 HUD 实时显示
// 事件源: fall/flasher/events {type:ready/waiting/connected/progress/done/error}
// 心跳: fall/telemetry/oled-display {"fw":"1.0.0","uptime":N}（30s，监工地基：DSH 感知显示板在线）
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "network.h"
#include "oled.h"

static const char *TAG = "display";

#define MQTT_HOST "YOUR_SERVER_HOST"
#define MQTT_USER "YOUR_MQTT_USER"
#define MQTT_PASS "YOUR_MQTT_PASSWORD"
#define EV_TOPIC  "fall/flasher/events"

// ---- 显示状态 ----
typedef enum {
    ST_IDLE, ST_READY, ST_WAITING, ST_CONNECTED,
    ST_PROGRESS, ST_DONE, ST_ERROR, ST_NO_MQTT
} disp_state_t;

static volatile disp_state_t g_state = ST_IDLE;
static volatile int g_pct = 0;
static char g_device[24] = "-";
static char g_err[32] = "";
static char g_last[32] = "--:--:--";
static bool g_have_oled = false;

static void hud_render(void) {
    if (!g_have_oled) return;
    oled_clear();
    oled_text(0, 0, "EvoAgent Flasher");
    char line[32];
    snprintf(line, sizeof(line), "TGT:%s", g_device);
    oled_text(0, 1, line);
    oled_text(0, 7, g_last);

    switch (g_state) {
    case ST_IDLE:
        oled_text(0, 3, "Waiting events...");
        break;
    case ST_NO_MQTT:
        oled_text(0, 3, "MQTT disconnected");
        oled_text(0, 4, "reconnecting...");
        break;
    case ST_READY:
        oled_text(0, 3, "READY");
        oled_text(0, 4, "flasher online");
        break;
    case ST_WAITING:
        oled_text(0, 3, "WAITING TARGET");
        oled_text(0, 4, "plug board + BOOT");
        oled_text(0, 5, "enter download mode");
        break;
    case ST_CONNECTED:
        oled_text(0, 3, "CONNECTED");
        oled_text(0, 4, "target found!");
        break;
    case ST_PROGRESS:
        oled_text(0, 3, "FLASHING");
        oled_bar(0, 4, 128, 8, (uint8_t)g_pct);
        snprintf(line, sizeof(line), "%d%%", g_pct);
        oled_text(0, 6, line);
        break;
    case ST_DONE:
        oled_text(0, 3, "== DONE ==");
        oled_text(0, 4, "firmware written");
        oled_text(0, 5, "MD5 verified");
        break;
    case ST_ERROR:
        oled_text(0, 3, "== ERROR ==");
        oled_text(0, 4, g_err);
        break;
    }
    oled_show();
}

static void fmt_time(char out[32]) {
    int64_t sec = esp_timer_get_time() / 1000000;
    snprintf(out, 32, "%02lld:%02lld:%02lld",
             (long long)(sec / 3600), (long long)((sec / 60) % 60), (long long)(sec % 60));
}

static void on_event(const char *payload) {
    cJSON *root = cJSON_Parse(payload);
    if (!root) return;
    const char *type = cJSON_GetObjectItem(root, "type") ? cJSON_GetObjectItem(root, "type")->valuestring : "";
    const char *dev = cJSON_GetObjectItem(root, "device") ? cJSON_GetObjectItem(root, "device")->valuestring : "";
    if (dev[0]) snprintf(g_device, sizeof(g_device), "%s", dev);

    if (!strcmp(type, "ready")) {
        g_state = ST_READY;
    } else if (!strcmp(type, "waiting")) {
        g_state = ST_WAITING;
    } else if (!strcmp(type, "connected")) {
        g_state = ST_CONNECTED;
    } else if (!strcmp(type, "progress")) {
        g_state = ST_PROGRESS;
        cJSON *p = cJSON_GetObjectItem(root, "pct");
        if (p) g_pct = p->valueint;
    } else if (!strcmp(type, "done")) {
        g_state = ST_DONE;
    } else if (!strcmp(type, "error")) {
        g_state = ST_ERROR;
        cJSON *e = cJSON_GetObjectItem(root, "err");
        if (e && e->valuestring) snprintf(g_err, sizeof(g_err), "%s", e->valuestring);
        else snprintf(g_err, sizeof(g_err), "unknown");
    }
    fmt_time(g_last);
    ESP_LOGI(TAG, "事件 %s: %s", type, payload);
    cJSON_Delete(root);
    hud_render();
}

static void mqtt_event(void *h, esp_event_base_t b, int32_t id, void *d) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)d;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接，订阅 %s", EV_TOPIC);
        esp_mqtt_client_subscribe(ev->client, EV_TOPIC, 0);
        g_state = ST_IDLE;
        fmt_time(g_last);
        hud_render();
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_state = ST_NO_MQTT;
        fmt_time(g_last);
        hud_render();
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(EV_TOPIC) && memcmp(ev->topic, EV_TOPIC, ev->topic_len) == 0) {
            char buf[256];
            int n = ev->data_len < (int)sizeof(buf) - 1 ? ev->data_len : (int)sizeof(buf) - 1;
            memcpy(buf, ev->data, n);
            buf[n] = 0;
            on_event(buf);
        }
        break;
    default:
        break;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "== OLED 显示板启动 ==");
    // 板载 WS2812（GPIO48）数据线拉低熄灭（默认高阻会显示异常颜色）
    gpio_set_direction(GPIO_NUM_48, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_48, 0);
    g_have_oled = oled_init();
    if (!g_have_oled) {
        ESP_LOGW(TAG, "OLED 未连接");
    }
    g_state = ST_IDLE;
    hud_render();

    network_init();
    if (!network_wait_connected(30000)) {
        ESP_LOGE(TAG, "WiFi 连接失败");
    } else {
        ESP_LOGI(TAG, "WiFi 就绪");
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtt://" MQTT_HOST,
        .broker.address.port = 1883,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
    };
    esp_mqtt_client_handle_t mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(mqtt);

    // 心跳：30s 周期上报（DSH 监工感知显示板在线）
    int64_t last_hb = 0;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000000;
        if (now - last_hb >= 30) {
            last_hb = now;
            char hb[96];
            snprintf(hb, sizeof(hb), "{\"device\":\"oled-display\",\"fw\":\"1.0.0\",\"uptime\":%lld}",
                     (long long)now);
            esp_mqtt_client_publish(mqtt, "fall/telemetry/oled-display", hb, 0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
