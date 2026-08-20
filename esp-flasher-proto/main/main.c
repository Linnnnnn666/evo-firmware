// 云端烧录板 v13（MQTT 指令执行器 + 心跳）：v12 基线 + 周期心跳上报
// 指令: fall/commands/flasher-board
//   {"cmd":"flash_start","device":"<id>"}          → 从 /firmware/<id>/latest.json 解析后烧录
//   {"cmd":"flash_start","url":"<bin>","size":N}   → 直接烧录指定 URL
//   {"cmd":"abort"}                                 → 中断当前流程（连接重试/烧录循环中生效）
//   {"cmd":"status"}                                → 回报当前状态
// 事件: fall/flasher/events {type:ready/waiting/connected/progress/done/error/status}
// 日志: fall/flasher/log {"device":"flasher-board","line":"..."}
//       （自带 flasher/net/mqtt 日志 tee + 烧录后目标板 UART 回显 TGT> 前缀）
// 心跳: fall/telemetry/flasher-board {"fw":"v13","state":"...","uptime":N,"tgt":"..."}（30s）
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp32_port.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "network.h"

static const char *TAG = "flasher";

#define SERVER_BASE   "http://YOUR_SERVER_HOST"
#define FLASHER_ID    "flasher-board"
#define CMD_TOPIC     "fall/commands/" FLASHER_ID
#define LOG_TOPIC     "fall/flasher/log"
#define MQTT_HOST     "YOUR_SERVER_HOST"
#define MQTT_USER     "YOUR_MQTT_USER"
#define MQTT_PASS     "YOUR_MQTT_PASSWORD"
#define EV_TOPIC      "fall/flasher/events"

#define HIGHER_BAUDRATE 230400
#define FLASH_BLOCK     4096
#define LED_GPIO        GPIO_NUM_48

#define TGT_UART        UART_NUM_1
#define TGT_TX_PIN      GPIO_NUM_1
#define TGT_RX_PIN      GPIO_NUM_2
#define TGT_ECHO_SECS   60

// ---- 运行状态 ----
typedef enum {
    ST_IDLE, ST_RESOLVE, ST_WAITING, ST_CONNECTED, ST_FLASHING, ST_ECHO
} run_state_t;

static const char *state_name(run_state_t s) {
    switch (s) {
    case ST_IDLE:      return "idle";
    case ST_RESOLVE:   return "resolve";
    case ST_WAITING:   return "waiting";
    case ST_CONNECTED: return "connected";
    case ST_FLASHING:  return "flashing";
    case ST_ECHO:      return "echo";
    }
    return "?";
}

static volatile run_state_t g_state = ST_IDLE;
static char g_target[24] = "-";

// ---- 指令槽（MQTT 回调写入，执行循环读取）----
static volatile bool g_cmd_ready = false;
static char g_cmd_payload[256];
static volatile bool s_abort = false;

// ---- MQTT ----
static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool s_mqtt_ok = false;

// ---- LED 指示（空闲亮，烧录中快闪，完成亮，出错慢闪）----
static void led_init(void) {
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 1);
}
static void led_on(void) { gpio_set_level(LED_GPIO, 1); }
static void led_off(void) { gpio_set_level(LED_GPIO, 0); }
static void led_blink(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        led_off(); vTaskDelay(pdMS_TO_TICKS(delay_ms));
        led_on(); vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---- 日志回传 ----
static vprintf_like_t s_prev_vprintf = NULL;
static bool s_log_in = false;         // 防递归
static int64_t s_last_log_us = 0;

static void log_pub_line(const char *line) {
    if (!s_mqtt || !s_mqtt_ok || !line) return;
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "{\"device\":\"%s\",\"line\":\"", FLASHER_ID);
    for (const char *p = line; *p && n < (int)sizeof(buf) - 8; p++) {
        if (*p == '"' || *p == '\\') {
            buf[n++] = '\\';
            buf[n++] = *p;
        } else if (*p == '\n') {          // 裸换行非法，必须转义（2026-08-21 修复）
            buf[n++] = '\\';
            buf[n++] = 'n';
        } else if (*p == '\r') {
            buf[n++] = '\\';
            buf[n++] = 'r';
        } else if (*p == '\t') {
            buf[n++] = '\\';
            buf[n++] = 't';
        } else {
            buf[n++] = *p;
        }
    }
    buf[n++] = '"';
    buf[n++] = '}';
    buf[n] = 0;
    esp_mqtt_client_publish(s_mqtt, LOG_TOPIC, buf, 0, 0, 0);
}

// 日志 tee：保留串口输出，同时把关键标签的日志回传 MQTT（限速 80ms 一条）
static int log_tee(const char *fmt, va_list args) {
    int n = 0;
    if (s_prev_vprintf) {
        va_list copy;
        va_copy(copy, args);
        n = s_prev_vprintf(fmt, copy);
        va_end(copy);
    }
    if (!s_log_in && s_mqtt_ok) {
        char buf[240];
        int m = vsnprintf(buf, sizeof(buf), fmt, args);
        if (m > 0) {
            if (strstr(buf, "flasher:") || strstr(buf, "net:") || strstr(buf, "network")
                || strstr(buf, "mqtt:") || strstr(buf, "display:")) {
                int64_t now = esp_timer_get_time();
                if (now - s_last_log_us > 80000) {
                    s_log_in = true;
                    log_pub_line(buf);
                    s_log_in = false;
                    s_last_log_us = now;
                }
            }
        }
    }
    return n;
}

// ---- MQTT 事件上报 ----
static void ev_pub(const char *type, const char *detail) {
    if (!s_mqtt || !s_mqtt_ok) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"device\":\"%s\",\"t\":%llu",
                     type, g_target, (unsigned long long)(esp_timer_get_time() / 1000000));
    if (detail && detail[0]) {
        n += snprintf(buf + n, sizeof(buf) - n, ",%s", detail);
    }
    snprintf(buf + n, sizeof(buf) - n, "}");
    esp_mqtt_client_publish(s_mqtt, EV_TOPIC, buf, 0, 0, 0);
    ESP_LOGI(TAG, "事件: %s %s", type, detail ? detail : "");
}

// ---- 指令处理（MQTT 回调上下文）----
static void cmd_handle(const char *payload) {
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        log_pub_line("指令 JSON 解析失败");
        return;
    }
    const char *cmd = cJSON_GetObjectItem(root, "cmd")
                          ? cJSON_GetObjectItem(root, "cmd")->valuestring : "";
    if (!strcmp(cmd, "flash_start")) {
        if (g_state != ST_IDLE) {
            ev_pub("error", "\"err\":\"busy\"");
            ESP_LOGW(TAG, "flash_start 被拒：当前状态 %s", state_name(g_state));
        } else {
            strlcpy(g_cmd_payload, payload, sizeof(g_cmd_payload));
            g_cmd_ready = true;
            s_abort = false;
            ESP_LOGI(TAG, "收到 flash_start: %s", payload);
        }
    } else if (!strcmp(cmd, "abort")) {
        s_abort = true;
        ESP_LOGI(TAG, "收到 abort");
    } else if (!strcmp(cmd, "status")) {
        char d[80];
        snprintf(d, sizeof(d), "\"state\":\"%s\",\"ver\":\"v12\",\"tgt\":\"%s\"",
                 state_name(g_state), g_target);
        ev_pub("status", d);
    } else {
        ESP_LOGW(TAG, "未知指令: %s", cmd);
    }
    cJSON_Delete(root);
}

static void mqtt_event(void *h, esp_event_base_t b, int32_t id, void *d) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)d;
    if ((esp_mqtt_event_id_t)id == MQTT_EVENT_CONNECTED) {
        s_mqtt_ok = true;
        ESP_LOGI(TAG, "MQTT 已连接，订阅 %s", CMD_TOPIC);
        esp_mqtt_client_subscribe(ev->client, CMD_TOPIC, 1);
    } else if ((esp_mqtt_event_id_t)id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_ok = false;
    } else if ((esp_mqtt_event_id_t)id == MQTT_EVENT_DATA) {
        if (ev->topic_len == (int)strlen(CMD_TOPIC)
            && memcmp(ev->topic, CMD_TOPIC, ev->topic_len) == 0) {
            char buf[256];
            int n = ev->data_len < (int)sizeof(buf) - 1 ? ev->data_len : (int)sizeof(buf) - 1;
            memcpy(buf, ev->data, n);
            buf[n] = 0;
            cmd_handle(buf);
        }
    }
}

static void mqtt_start(void) {
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtt://" MQTT_HOST,
        .broker.address.port = 1883,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_mqtt);
}

// ---- 连接目标（无限重试，abort 可中断）----
static esp_loader_error_t connect_to_target(esp_loader_t *loader, uint32_t higher_rate) {
    esp_loader_connect_args_t cfg = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = esp_loader_connect(loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) return err;
    ESP_LOGI(TAG, "已连接目标（ROM loader）");
    if (higher_rate && esp_loader_get_target(loader) != ESP8266_CHIP) {
        err = esp_loader_change_transmission_rate(loader, higher_rate);
        if (err != ESP_LOADER_SUCCESS && err != ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
            ESP_LOGW(TAG, "提升速率失败 err=%d", err);
        }
    }
    return ESP_LOADER_SUCCESS;
}

static bool http_get_small(const char *url, char *buf, size_t len) {
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 10000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_open(c, 0);
    bool ok = false;
    if (err == ESP_OK) {
        int cl = esp_http_client_fetch_headers(c);
        int total = 0;
        if (cl >= 0) {
            while (total < (int)len - 1) {
                int r = esp_http_client_read(c, buf + total, len - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = 0;
            ok = (total > 0);
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

// ---- 流式烧录：下载 full.bin 边读边写，进度上报，abort 可中断 ----
static esp_loader_error_t flash_from_http(esp_loader_t *loader, const char *url, uint32_t expected_size) {
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_LOADER_ERROR_FAIL;

    esp_loader_error_t result = ESP_LOADER_ERROR_FAIL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ev_pub("error", "\"err\":\"http_open\"");
        esp_http_client_cleanup(c);
        return result;
    }
    int cl = esp_http_client_fetch_headers(c);
    ESP_LOGI(TAG, "HTTP 下载: %s (%d B)", url, cl);
    if (cl > 0 && expected_size > 0 && cl != (int)expected_size) {
        ev_pub("error", "\"err\":\"size_mismatch\"");
        esp_http_client_cleanup(c);
        return result;
    }

    esp_loader_flash_cfg_t wcfg = {
        .offset = 0x0,
        .image_size = (uint32_t)(cl > 0 ? cl : expected_size),
        .block_size = FLASH_BLOCK,
        .skip_verify = false,
    };
    result = esp_loader_flash_start(loader, &wcfg);
    if (result != ESP_LOADER_SUCCESS) {
        char d[64];
        snprintf(d, sizeof(d), "\"err\":\"flash_start_%d\"", result);
        ev_pub("error", d);
        esp_http_client_cleanup(c);
        return result;
    }

    uint8_t *block = malloc(FLASH_BLOCK);
    if (!block) {
        ev_pub("error", "\"err\":\"oom\"");
        esp_http_client_cleanup(c);
        return ESP_LOADER_ERROR_FAIL;
    }
    uint32_t written = 0;
    int last_pct = -1;
    for (;;) {
        if (s_abort) {
            ev_pub("error", "\"err\":\"aborted\"");
            result = ESP_LOADER_ERROR_FAIL;
            break;
        }
        int r = esp_http_client_read(c, (char *)block, FLASH_BLOCK);
        if (r < 0) {
            ev_pub("error", "\"err\":\"http_read\"");
            result = ESP_LOADER_ERROR_FAIL;
            break;
        }
        if (r == 0) break;
        result = esp_loader_flash_write(loader, &wcfg, block, (uint32_t)r);
        if (result != ESP_LOADER_SUCCESS) {
            char d[64];
            snprintf(d, sizeof(d), "\"err\":\"write_%d\"", result);
            ev_pub("error", d);
            break;
        }
        written += (uint32_t)r;
        int pct = (int)(written * 100 / wcfg.image_size);
        if (pct != last_pct && pct % 10 == 0) {
            ESP_LOGI(TAG, "烧录进度 %d%%", pct);
            char d[96];
            snprintf(d, sizeof(d), "\"pct\":%d,\"written\":%lu,\"total\":%lu",
                     pct, (unsigned long)written, (unsigned long)wcfg.image_size);
            ev_pub("progress", d);
            led_blink(2, 60);  // 烧录中快闪指示
            last_pct = pct;
        }
    }
    free(block);

    if (result == ESP_LOADER_SUCCESS) {
        result = esp_loader_flash_finish(loader, &wcfg);
        if (result == ESP_LOADER_SUCCESS) {
            ESP_LOGI(TAG, "烧录完成 %u B, MD5 通过", written);
            char d[96];
            snprintf(d, sizeof(d), "\"bytes\":%lu", (unsigned long)written);
            ev_pub("done", d);
            led_on();
        } else {
            char d[64];
            snprintf(d, sizeof(d), "\"err\":\"md5_%d\"", result);
            ev_pub("error", d);
            led_blink(5, 200);  // 出错慢闪
        }
    }
    esp_http_client_cleanup(c);
    return result;
}

// ---- 目标板日志回显（烧录成功后监听目标 UART 60s，TGT> 前缀回传）----
static void echo_target_logs(int secs) {
    uart_config_t uc = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(TGT_UART, 2048, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "UART1 回显初始化失败");
        return;
    }
    uart_param_config(TGT_UART, &uc);
    uart_set_pin(TGT_UART, TGT_TX_PIN, TGT_RX_PIN, -1, -1);
    ESP_LOGI(TAG, "目标日志回显 %ds（按目标板 RST 可看到启动日志）", secs);
    char line[240];
    int li = 0;
    uint8_t b;
    int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
    while (esp_timer_get_time() < end) {
        if (uart_read_bytes(TGT_UART, &b, 1, 100) == 1) {
            if (b == '\n' || li >= (int)sizeof(line) - 2) {
                line[li] = 0;
                if (li > 0) {
                    char tgt[256];
                    snprintf(tgt, sizeof(tgt), "TGT> %s", line);
                    log_pub_line(tgt);
                }
                li = 0;
            } else if (b >= 32 || b == '\t') {
                line[li++] = (char)b;
            }
        }
    }
    uart_driver_delete(TGT_UART);
    ESP_LOGI(TAG, "目标日志回显结束");
}

// ---- 执行一次烧录任务（由 flash_start 指令触发）----
static void back_to_idle(void) {
    g_state = ST_IDLE;
    ev_pub("ready", NULL);  // 通知显示板/监工：执行器已回空闲
}

static void run_flash_job(void) {
    g_cmd_ready = false;
    char cmd[256];
    memcpy(cmd, g_cmd_payload, sizeof(cmd));
    cmd[sizeof(cmd) - 1] = 0;

    cJSON *root = cJSON_Parse(cmd);
    if (!root) {
        ev_pub("error", "\"err\":\"bad_cmd\"");
        return;
    }
    const char *device = cJSON_GetObjectItem(root, "device")
                             ? cJSON_GetObjectItem(root, "device")->valuestring : NULL;
    const char *url = cJSON_GetObjectItem(root, "url")
                          ? cJSON_GetObjectItem(root, "url")->valuestring : NULL;
    uint32_t size = cJSON_GetObjectItem(root, "size")
                        ? (uint32_t)cJSON_GetObjectItem(root, "size")->valuedouble : 0;
    if (!device && !url) {
        ev_pub("error", "\"err\":\"no_device\"");
        cJSON_Delete(root);
        back_to_idle();
        return;
    }
    // 先拷出再用（cJSON_Delete 后指针失效，严禁 use-after-free）
    char dev_buf[24] = "";
    char url_buf[192] = "";
    if (device) strlcpy(dev_buf, device, sizeof(dev_buf));
    if (url) strlcpy(url_buf, url, sizeof(url_buf));
    if (dev_buf[0]) strlcpy(g_target, dev_buf, sizeof(g_target));
    else strlcpy(g_target, "direct", sizeof(g_target));
    cJSON_Delete(root);
    device = dev_buf[0] ? dev_buf : NULL;
    url = url_buf[0] ? url_buf : NULL;

    g_state = ST_RESOLVE;
    char full_url[192];
    uint32_t fsize = 0;
    if (device) {
        char latest_url[160];
        snprintf(latest_url, sizeof(latest_url), SERVER_BASE "/firmware/%s/latest.json", device);
        static char latest[1024];
        memset(latest, 0, sizeof(latest));
        if (!http_get_small(latest_url, latest, sizeof(latest))) {
            ev_pub("error", "\"err\":\"no_latest\"");
            back_to_idle();
            return;
        }
        cJSON *lroot = cJSON_Parse(latest);
        cJSON *full = lroot ? cJSON_GetObjectItem(lroot, "full") : NULL;
        const char *bin = full && cJSON_GetObjectItem(full, "bin")
                              ? cJSON_GetObjectItem(full, "bin")->valuestring : NULL;
        if (!bin) {
            if (lroot) cJSON_Delete(lroot);
            ev_pub("error", "\"err\":\"no_full\"");
            back_to_idle();
            return;
        }
        fsize = full && cJSON_GetObjectItem(full, "size")
                    ? (uint32_t)cJSON_GetObjectItem(full, "size")->valuedouble : 0;
        snprintf(full_url, sizeof(full_url), SERVER_BASE "/firmware/%s/%s", device, bin);
        if (lroot) cJSON_Delete(lroot);
    } else {
        strlcpy(full_url, url, sizeof(full_url));
        fsize = size;
    }
    if (s_abort) {
        ev_pub("error", "\"err\":\"aborted\"");
        s_abort = false;
        back_to_idle();
        return;
    }
    ESP_LOGI(TAG, "云端固件: %s (%u B)", g_target, fsize);

    // 烧录串口（GPIO1/2 UART）
    esp_loader_t loader;
    esp32_port_t port = {
        .port.ops  = &esp32_uart_ops,
        .baud_rate = 115200,
        .uart_port = TGT_UART,
        .uart_rx_pin = TGT_RX_PIN,
        .uart_tx_pin = TGT_TX_PIN,
        .reset_pin   = GPIO_NUM_NC,
        .boot_pin    = GPIO_NUM_NC,
    };
    if (esp_loader_init_serial(&loader, &port.port) != ESP_LOADER_SUCCESS) {
        ev_pub("error", "\"err\":\"uart_init\"");
        back_to_idle();
        return;
    }

    // 连接目标（无限重试，abort 可中断）
    g_state = ST_WAITING;
    ev_pub("waiting", NULL);
    bool connected = false;
    for (int attempt = 1; !connected && !s_abort; attempt++) {
        if (connect_to_target(&loader, HIGHER_BAUDRATE) == ESP_LOADER_SUCCESS) {
            connected = true;
        } else {
            ESP_LOGW(TAG, "连接失败 %d 次，重试...", attempt);
            led_blink(1, 100);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    if (s_abort) {
        ev_pub("error", "\"err\":\"aborted\"");
        s_abort = false;
        esp_loader_deinit(&loader);
        back_to_idle();
        return;
    }
    ev_pub("connected", NULL);
    g_state = ST_CONNECTED;

    // 下载 + 烧录
    g_state = ST_FLASHING;
    esp_loader_error_t err = flash_from_http(&loader, full_url, fsize);
    if (err != ESP_LOADER_SUCCESS) {
        if (s_abort) s_abort = false;  // abort 错误已在 flash_from_http 内上报
        led_blink(5, 200);
    }
    esp_loader_deinit(&loader);

    // 目标日志回显（仅烧录成功）
    if (err == ESP_LOADER_SUCCESS) {
        g_state = ST_ECHO;
        echo_target_logs(TGT_ECHO_SECS);
    }

    g_state = ST_IDLE;
    ev_pub("ready", NULL);
}

// ---- 心跳（30s）：DSH 监工感知板子在线（fall/telemetry/flasher-board）----
#define HB_TOPIC "fall/telemetry/flasher-board"
static void hb_pub(void) {
    if (!s_mqtt || !s_mqtt_ok) return;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"%s\",\"fw\":\"v13\",\"state\":\"%s\",\"uptime\":%llu,\"tgt\":\"%s\"}",
             FLASHER_ID, state_name(g_state),
             (unsigned long long)(esp_timer_get_time() / 1000000), g_target);
    esp_mqtt_client_publish(s_mqtt, HB_TOPIC, buf, 0, 0, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "== 云端烧录板 v13（MQTT 指令执行器 + 心跳）启动 ==");
    s_prev_vprintf = esp_log_set_vprintf(log_tee);
    led_init();
    led_on();

    network_init();
    if (!network_wait_connected(30000)) {
        ESP_LOGE(TAG, "WiFi 连接失败");
    } else {
        ESP_LOGI(TAG, "WiFi 就绪");
    }
    mqtt_start();
    vTaskDelay(pdMS_TO_TICKS(3000));  // 等 MQTT 连接
    ev_pub("ready", NULL);

    for (;;) {
        if (g_cmd_ready) {
            run_flash_job();
        }
        // 心跳：30s 周期上报（监工地基）
        static int64_t s_last_hb = 0;
        int64_t hb_now = esp_timer_get_time() / 1000000;
        if (hb_now - s_last_hb >= 30) {
            s_last_hb = hb_now;
            hb_pub();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
