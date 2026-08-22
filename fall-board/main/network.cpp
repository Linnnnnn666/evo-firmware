/*
 * 网络层：异步可靠上报（任务书 6.1/6.3）
 *
 * 新链路（FALL_ENABLE_DIRECT_SERVER=1，默认）：
 *   fall_network_submit_fall_event()  → 生成 event_id/device_seq，入 RAM 队列，
 *                                       追加 NVS 持久化队列，立即返回。
 *   独立 network_task：
 *     - 等待 Wi-Fi（不阻塞推理任务）
 *     - 按 FIFO 发送新 API：POST /api/v1/devices/{id}/events（Bearer token）
 *     - 2xx 且匹配 event_id → 从 NVS 队列删除（只补传一次）
 *     - 4xx → 记录并丢弃（重试无意义）；网络错误/5xx → 指数退避重试
 *     - 退避：2s/5s/10s/30s/60s/5min + 随机抖动
 *     - 空闲时每 5 分钟发送心跳（失败不重试，可丢弃）
 *   SNTP 校时：Wi-Fi 连通后启动；未校时 device_time 为空。
 *   重启后从 NVS 恢复未确认事件，按时间顺序补传。
 *
 * 旧链路（FALL_ENABLE_DIRECT_SERVER=0）：保留原同步实现作为回退。
 */
#include "network.h"
#include "device_config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"   // esp_reset_reason()
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"   // heap_caps_get_free_size
#include "nvs_flash.h"

#include "cJSON.h"

static const char *TAG = "fall_net";

/* ---------------- Wi-Fi 状态 ---------------- */
static EventGroupHandle_t g_wifi_events = nullptr;
static bool g_wifi_started = false;
static bool g_wifi_connected = false;
static int g_wifi_retries = 0;
static bool g_sntp_started = false;

static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

/* ---------------- 异步发送 ---------------- */
static QueueHandle_t g_event_queue = nullptr;      // RAM 队列（FallEvent 值拷贝）
static SemaphoreHandle_t g_nvs_lock = nullptr;

static constexpr int kQueueDepth = 16;             // RAM 队列深度
static constexpr int kMaxNvsEvents = 6;            // NVS 持久化上限（24KB 分区，单 blob < 4KB）
static constexpr int kSendTimeoutMs = 8000;        // 单次请求连接+读取超时
static constexpr uint32_t kHeartbeatPeriodS = 300; // 心跳周期（任务书 4.3）
static constexpr char kNvsNamespace[] = "fall";
static constexpr char kNvsSeqKey[] = "seq";        // u32 单调递增
static constexpr char kNvsQueueKey[] = "queue";    // blob: JSON 数组

// 指数退避序列（任务书 6.1），末级 5 分钟后封顶
static constexpr uint32_t kBackoffMs[] = {2000, 5000, 10000, 30000, 60000, 300000};
static constexpr int kBackoffLevels = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

// 事件快照（RAM 队列 + NVS JSON 共用字段）
struct FallEvent {
    char event_id[37];
    uint32_t seq;
    float radar_x, radar_y, radar_z;
    int fall_votes, total_frames;
    uint32_t uptime_ms;
    int64_t device_time_epoch;  // 0 = 尚未校时
};

/* ---------------- 工具 ---------------- */

static void make_uuid4(char out[37]) {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t r = esp_random();
        memcpy(b + i, &r, 4);
    }
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static uint32_t jitter_ms(uint32_t base_ms) {
    // ±20% 随机抖动，避免设备同步重试
    const int32_t delta = static_cast<int32_t>(base_ms / 5) *
                          (static_cast<int32_t>(esp_random() % 201) - 100) / 100;
    return static_cast<uint32_t>(base_ms + delta);
}

static int64_t now_epoch_ms() {
    return static_cast<int64_t>(esp_timer_get_time() / 1000);
}

// 校时判定：2025-01-01 之后才算已校时
static bool time_synced() {
    return time(nullptr) > 1735689600;
}

static void format_device_time(char out[32], size_t len) {
    if (!time_synced()) {
        out[0] = '\0';
        return;
    }
    const time_t t = time(nullptr);
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    // tm fields are int; GCC cannot prove they fit %02d, so silence the
    // static-analysis truncation warning here and guard with the return value.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    const int n = snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
                           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#pragma GCC diagnostic pop
    if (n < 0 || (size_t)n >= len) {
        out[len - 1] = '\0';  // 防御性截断
    }
}

/* ---------------- NVS 持久化队列 ---------------- */

static bool nvs_load_queue(FallEvent out[], int *count) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    char *buf = static_cast<char *>(malloc(3968));
    if (buf == nullptr) { nvs_close(h); return false; }
    size_t len = 3968;
    const esp_err_t err = nvs_get_blob(h, kNvsQueueKey, buf, &len);
    nvs_close(h);
    *count = 0;
    if (err == ESP_ERR_NVS_NOT_FOUND) { free(buf); return true; }
    if (err != ESP_OK) { free(buf); return false; }

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (arr == nullptr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return false;
    }
    int n = 0;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if (n >= kMaxNvsEvents) break;
        cJSON *j = it;
        const char *eid = cJSON_GetObjectItem(j, "event_id") ? cJSON_GetObjectItem(j, "event_id")->valuestring : nullptr;
        if (!eid) continue;
        snprintf(out[n].event_id, sizeof(out[n].event_id), "%s", eid);
        out[n].seq = cJSON_GetObjectItem(j, "seq") ? static_cast<uint32_t>(cJSON_GetObjectItem(j, "seq")->valuedouble) : 0;
        out[n].radar_x = cJSON_GetObjectItem(j, "x") ? static_cast<float>(cJSON_GetObjectItem(j, "x")->valuedouble) : 0.0f;
        out[n].radar_y = cJSON_GetObjectItem(j, "y") ? static_cast<float>(cJSON_GetObjectItem(j, "y")->valuedouble) : 0.0f;
        out[n].radar_z = cJSON_GetObjectItem(j, "z") ? static_cast<float>(cJSON_GetObjectItem(j, "z")->valuedouble) : 0.0f;
        out[n].fall_votes = cJSON_GetObjectItem(j, "votes") ? cJSON_GetObjectItem(j, "votes")->valueint : 0;
        out[n].total_frames = cJSON_GetObjectItem(j, "frames") ? cJSON_GetObjectItem(j, "frames")->valueint : 0;
        out[n].uptime_ms = cJSON_GetObjectItem(j, "uptime") ? static_cast<uint32_t>(cJSON_GetObjectItem(j, "uptime")->valuedouble) : 0;
        out[n].device_time_epoch = cJSON_GetObjectItem(j, "t") ? static_cast<int64_t>(cJSON_GetObjectItem(j, "t")->valuedouble) : 0;
        n++;
    }
    cJSON_Delete(arr);
    *count = n;
    return true;
}

// 把内存队列整体写回 NVS（保留最新 kMaxNvsEvents 条，丢弃最旧）
static bool nvs_save_queue(const FallEvent *events, int count) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == nullptr) return false;
    const int start = count > kMaxNvsEvents ? count - kMaxNvsEvents : 0;
    for (int i = start; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event_id", events[i].event_id);
        cJSON_AddNumberToObject(o, "seq", events[i].seq);
        cJSON_AddNumberToObject(o, "x", events[i].radar_x);
        cJSON_AddNumberToObject(o, "y", events[i].radar_y);
        cJSON_AddNumberToObject(o, "z", events[i].radar_z);
        cJSON_AddNumberToObject(o, "votes", events[i].fall_votes);
        cJSON_AddNumberToObject(o, "frames", events[i].total_frames);
        cJSON_AddNumberToObject(o, "uptime", events[i].uptime_ms);
        cJSON_AddNumberToObject(o, "t", static_cast<double>(events[i].device_time_epoch));
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json == nullptr) return false;

    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        const esp_err_t err = nvs_set_blob(h, kNvsQueueKey, json, strlen(json) + 1);
        ok = (err == ESP_OK) && (nvs_commit(h) == ESP_OK);
        nvs_close(h);
    }
    free(json);
    return ok;
}

// 追加一条事件到持久化队列（满则丢最旧）
static void nvs_append_event(const FallEvent &ev) {
    if (g_nvs_lock == nullptr) return;
    if (xSemaphoreTake(g_nvs_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    FallEvent tmp[kMaxNvsEvents + 1];
    int n = 0;
    if (nvs_load_queue(tmp, &n)) {
        if (n < kMaxNvsEvents) {
            tmp[n++] = ev;
        } else {
            // 满：丢最旧，保最新（任务书 6.1：队列满时优先保留跌倒事件）
            for (int i = 1; i < n; i++) tmp[i - 1] = tmp[i];
            tmp[n - 1] = ev;
        }
        nvs_save_queue(tmp, n);
    } else {
        tmp[0] = ev;
        nvs_save_queue(tmp, 1);
    }
    xSemaphoreGive(g_nvs_lock);
}

// 发送成功后从持久化队列删除该 event_id（只补传一次的保证）
static void nvs_remove_event(const char *event_id) {
    if (g_nvs_lock == nullptr || event_id == nullptr) return;
    if (xSemaphoreTake(g_nvs_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    FallEvent tmp[kMaxNvsEvents];
    int n = 0;
    if (nvs_load_queue(tmp, &n)) {
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(tmp[i].event_id, event_id) != 0) tmp[w++] = tmp[i];
        }
        if (w != n) nvs_save_queue(tmp, w);
    }
    xSemaphoreGive(g_nvs_lock);
}

static uint32_t nvs_next_seq() {
    nvs_handle_t h;
    uint32_t seq = 1;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, kNvsSeqKey, &seq);
        seq++;
        nvs_set_u32(h, kNvsSeqKey, seq);
        nvs_commit(h);
        nvs_close(h);
    }
    return seq;
}

/* ---------------- HTTP 发送 ---------------- */

static char url_buf[192];

static const char *events_url() {
    snprintf(url_buf, sizeof(url_buf), "%s/api/v1/devices/%s/events",
             DEVICE_API_BASE_URL, DEVICE_ID);
    return url_buf;
}

// 组新 API 事件体（任务书 4.1）
static char *build_event_body(const FallEvent &ev) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event_id", ev.event_id);
    cJSON_AddStringToObject(o, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(o, "device_seq", ev.seq);
    cJSON_AddStringToObject(o, "event_type", "fall_confirmed");
    cJSON_AddStringToObject(o, "firmware_version", FALL_FW_VERSION);
    cJSON_AddStringToObject(o, "fw_version", FALL_FW_VERSION);
    cJSON_AddStringToObject(o, "model_version", FALL_MODEL_VERSION);
    cJSON *radar = cJSON_CreateObject();
    cJSON_AddNumberToObject(radar, "x", ev.radar_x);
    cJSON_AddNumberToObject(radar, "y", ev.radar_y);
    cJSON_AddNumberToObject(radar, "z", ev.radar_z);
    cJSON_AddItemToObject(o, "radar", radar);
    cJSON *vision = cJSON_CreateObject();
    cJSON_AddNumberToObject(vision, "fall_votes", ev.fall_votes);
    cJSON_AddNumberToObject(vision, "total_frames", ev.total_frames);
    cJSON_AddItemToObject(o, "vision", vision);
    cJSON_AddNumberToObject(o, "device_uptime_ms", ev.uptime_ms);
    char tbuf[32];
    format_device_time(tbuf, sizeof(tbuf));
    if (tbuf[0] != '\0') {
        cJSON_AddStringToObject(o, "device_time", tbuf);
    }
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;  // caller frees
}

// 返回：0=成功(2xx)，1=永久失败(4xx，应丢弃)，-1=可重试(网络/5xx)
static int http_post_event(const FallEvent &ev, int *http_status) {
    char *body = build_event_body(ev);
    if (body == nullptr) return -1;

    esp_http_client_config_t cfg = {};
    cfg.url = events_url();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kSendTimeoutMs;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        free(body);
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " DEVICE_TOKEN);
    esp_http_client_set_header(client, "X-Event-Id", ev.event_id);
    esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    *http_status = status;
    if (err == ESP_OK && status >= 200 && status < 300) return 0;
    if (err == ESP_OK && status >= 400 && status < 500) return 1;  // 4xx 永久
    return -1;  // 网络错误 / 5xx / 超时
}

static char hb_url_buf[192];
static const char *heartbeat_url() {
    snprintf(hb_url_buf, sizeof(hb_url_buf), "%s/api/v1/devices/%s/heartbeat",
             DEVICE_API_BASE_URL, DEVICE_ID);
    return hb_url_buf;
}

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "POWERON_RESET";
        case ESP_RST_SW: return "SW_RESET";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP_RESET";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        default: return "UNKNOWN";
    }
}

// 心跳（任务书 4.3）。失败不重试，下次周期再发。
static void send_heartbeat() {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware_version", FALL_FW_VERSION);
    cJSON_AddStringToObject(o, "model_version", FALL_MODEL_VERSION);
    cJSON_AddNumberToObject(o, "uptime_s", static_cast<double>(now_epoch_ms() / 1000));

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_rssi", ap.rssi);
    }
    cJSON_AddNumberToObject(o, "free_heap",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(o, "largest_free_block",
                            static_cast<double>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    cJSON_AddStringToObject(o, "reset_reason", reset_reason_str(esp_reset_reason()));

    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (body == nullptr) return;

    esp_http_client_config_t cfg = {};
    cfg.url = heartbeat_url();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kSendTimeoutMs;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) { free(body); return; }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " DEVICE_TOKEN);
    esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "heartbeat status=%d err=%s", status, esp_err_to_name(err));
    esp_http_client_cleanup(client);
    free(body);
}

/* ---------------- network_task ---------------- */

static void start_sntp() {
    if (g_sntp_started) return;
    g_sntp_started = true;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        1, ESP_SNTP_SERVER_LIST("ntp.aliyun.com"));  // 国内可达
    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
    }
}

// 重启恢复：把 NVS 中未确认事件按顺序补入 RAM 队列（先入先发）
static void restore_pending_events() {
    FallEvent evs[kMaxNvsEvents];
    int n = 0;
    if (!nvs_load_queue(evs, &n) || n == 0) return;
    ESP_LOGI(TAG, "restoring %d pending event(s) from NVS", n);
    for (int i = 0; i < n; i++) {
        if (xQueueSend(g_event_queue, &evs[i], pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "RAM queue full during restore; dropping oldest NVS entry");
            // 队列满：弹掉最旧再放（尽量保最新）
            FallEvent drop;
            if (xQueueReceive(g_event_queue, &drop, 0) == pdTRUE) {
                nvs_remove_event(drop.event_id);
                xQueueSend(g_event_queue, &evs[i], pdMS_TO_TICKS(100));
            }
        }
    }
}

static void network_task(void * /*arg*/) {
    restore_pending_events();

    int backoff_idx = 0;
    bool last_failed = false;
    uint32_t last_heartbeat_ms = 0;
    FallEvent ev;

    for (;;) {
        // 等 Wi-Fi（只阻塞本任务）
        xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        start_sntp();

        // 从 RAM 队列取事件；空闲超时触发心跳
        const uint32_t wait_ms = last_failed ? 0 : (kHeartbeatPeriodS * 1000);
        if (xQueueReceive(g_event_queue, &ev, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
#if FALL_ENABLE_HEARTBEAT
            const uint32_t now = static_cast<uint32_t>(now_epoch_ms());
            if (now - last_heartbeat_ms >= kHeartbeatPeriodS * 1000) {
                last_heartbeat_ms = now;
                send_heartbeat();
            }
#endif
            continue;
        }

        // 发送（同一 event_id 重试，body 不变）
        int status = 0;
        const int rc = http_post_event(ev, &status);
        if (rc == 0) {
            ESP_LOGI(TAG, "event %s (seq=%u) uploaded (status=%d)",
                     ev.event_id, ev.seq, status);
            nvs_remove_event(ev.event_id);
            last_failed = false;
            backoff_idx = 0;
        } else if (rc == 1) {
            ESP_LOGW(TAG, "event %s permanently rejected (status=%d); dropping",
                     ev.event_id, status);
            nvs_remove_event(ev.event_id);
            last_failed = false;
            backoff_idx = 0;
        } else {
            ESP_LOGW(TAG, "event %s (seq=%u) failed (status=%d); retry in %ums",
                     ev.event_id, ev.seq, status,
                     kBackoffMs[backoff_idx < kBackoffLevels ? backoff_idx : kBackoffLevels - 1]);
            // 保留事件：退回队列尾（等待下一轮重发），指数退避
            xQueueSend(g_event_queue, &ev, pdMS_TO_TICKS(50));
            last_failed = true;
            if (backoff_idx < kBackoffLevels - 1) backoff_idx++;
            vTaskDelay(pdMS_TO_TICKS(jitter_ms(kBackoffMs[backoff_idx - 1])));
        }
        // 让出 CPU 并避免阻塞其他任务
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------- 公共接口 ---------------- */

static void wifi_event_handler(void *,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        // 无限自动重连（修复原 20 次上限后不再重连的问题），降频日志
        if (++g_wifi_retries == 1 || g_wifi_retries % 10 == 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected (retry #%d); reconnecting...", g_wifi_retries);
        }
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *got_ip = static_cast<const ip_event_got_ip_t *>(event_data);
        g_wifi_retries = 0;
        g_wifi_connected = true;
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        printf("[WIFI] connected, IP=" IPSTR "\n", IP2STR(&got_ip->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

bool fall_network_init(void) {
    if (g_wifi_started) return true;

    if (DEVICE_WIFI_SSID[0] == '\0' ||
        strcmp(DEVICE_WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
        ESP_LOGW(TAG, "Wi-Fi is not configured; edit main/device_config_private.h");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
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
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return false;
    }

    static esp_netif_t *sta_netif = nullptr;
    if (sta_netif == nullptr) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == nullptr) {
            ESP_LOGE(TAG, "failed to create default Wi-Fi STA");
            return false;
        }
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (g_wifi_events == nullptr) {
        g_wifi_events = xEventGroupCreate();
        if (g_wifi_events == nullptr) {
            ESP_LOGE(TAG, "Wi-Fi event group allocation failed");
            return false;
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                                ESP_EVENT_ANY_ID,
                                                &wifi_event_handler,
                                                nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler,
                                                nullptr));

    wifi_config_t sta_config = {};
    strlcpy(reinterpret_cast<char *>(sta_config.sta.ssid),
            DEVICE_WIFI_SSID,
            sizeof(sta_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(sta_config.sta.password),
            DEVICE_WIFI_PASSWORD,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    g_wifi_started = true;
    printf("[WIFI] connecting to SSID: %s\n", DEVICE_WIFI_SSID);
    ESP_LOGI(TAG, "Wi-Fi station started, SSID=%s", DEVICE_WIFI_SSID);

#if FALL_ENABLE_DIRECT_SERVER
    // 异步链路：队列 + 网络任务
    if (g_event_queue == nullptr) {
        g_event_queue = xQueueCreate(kQueueDepth, sizeof(FallEvent));
        if (g_event_queue == nullptr) {
            ESP_LOGE(TAG, "event queue allocation failed");
            return false;
        }
    }
    if (g_nvs_lock == nullptr) {
        g_nvs_lock = xSemaphoreCreateMutex();
    }
    static bool task_created = false;
    if (!task_created) {
        if (xTaskCreate(network_task, "fall_net", 8192, nullptr, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "network task creation failed");
            return false;
        }
        task_created = true;
    }
    ESP_LOGI(TAG, "async network task ready (queue depth=%d, NVS cap=%d)",
             kQueueDepth, kMaxNvsEvents);
#endif
    return true;
}

bool fall_network_is_connected(void) {
    return g_wifi_connected &&
           g_wifi_events != nullptr &&
           (xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

uint32_t fall_network_pending_count(void) {
    if (g_event_queue == nullptr) return 0;
    return static_cast<uint32_t>(uxQueueMessagesWaiting(g_event_queue));
}

bool fall_network_submit_fall_event(float radar_x,
                                    float radar_y,
                                    float radar_z,
                                    int fall_votes,
                                    int total_frames) {
#if FALL_ENABLE_DIRECT_SERVER
    if (g_event_queue == nullptr) {
        ESP_LOGE(TAG, "submit before init");
        return false;
    }
    FallEvent ev = {};
    make_uuid4(ev.event_id);                    // 事件生成时固定，重试不重新生成
    ev.seq = nvs_next_seq();                    // NVS 单调递增（任务书 6.3）
    ev.radar_x = radar_x;
    ev.radar_y = radar_y;
    ev.radar_z = radar_z;
    ev.fall_votes = fall_votes;
    ev.total_frames = total_frames;
    ev.uptime_ms = static_cast<uint32_t>(now_epoch_ms());
    ev.device_time_epoch = static_cast<int64_t>(time(nullptr));

    nvs_append_event(ev);                       // 持久化：重启不丢失
    const BaseType_t queued =
        xQueueSend(g_event_queue, &ev, pdMS_TO_TICKS(100));
    if (queued != pdTRUE) {
        // RAM 队列满：弹最旧，保最新（任务书 6.1）
        FallEvent drop;
        if (xQueueReceive(g_event_queue, &drop, 0) == pdTRUE) {
            nvs_remove_event(drop.event_id);
            xQueueSend(g_event_queue, &ev, pdMS_TO_TICKS(100));
        }
        ESP_LOGW(TAG, "event %s enqueued after dropping oldest", ev.event_id);
    } else {
        ESP_LOGI(TAG, "event %s (seq=%u) enqueued (pending=%u)",
                 ev.event_id, ev.seq, fall_network_pending_count());
    }
    return true;  // 立即返回，不阻塞推理流程
#else
    // 旧链路回退：同步发送（原实现语义）
    return fall_network_post_result(radar_x, radar_y, radar_z,
                                    fall_votes, total_frames);
#endif
}

/* ================= 旧链路同步实现（FALL_ENABLE_DIRECT_SERVER=0 时使用） ================= */

bool fall_network_post_result(float radar_x,
                              float radar_y,
                              float radar_z,
                              int fall_votes,
                              int total_frames) {
    if (!fall_network_is_connected() && g_wifi_events != nullptr) {
        ESP_LOGI(TAG, "waiting up to 10 seconds for Wi-Fi before upload");
        xEventGroupWaitBits(g_wifi_events,
                            WIFI_CONNECTED_BIT,
                            pdFALSE,
                            pdTRUE,
                            pdMS_TO_TICKS(10000));
    }
    if (!fall_network_is_connected()) {
        ESP_LOGW(TAG, "upload skipped: Wi-Fi is not connected");
        return false;
    }

    char body[384];
    const int body_len = snprintf(
        body,
        sizeof(body),
        "{\"device_id\":\"%s\",\"event\":\"fall_confirmed\","
        "\"fw_version\":\"%s\","
        "\"radar_x\":%.3f,\"radar_y\":%.3f,\"radar_z\":%.3f,"
        "\"fall_votes\":%d,\"total_frames\":%d}",
        DEVICE_ID, FALL_FW_VERSION,
        static_cast<double>(radar_x),
        static_cast<double>(radar_y),
        static_cast<double>(radar_z),
        fall_votes,
        total_frames);
    if (body_len <= 0 || body_len >= static_cast<int>(sizeof(body))) {
        ESP_LOGE(TAG, "event JSON is too large");
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = DEVICE_API_BASE_URL "/api/fall";
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    config.keep_alive_enable = false;

    for (int attempt = 1; attempt <= 3; ++attempt) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ESP_LOGE(TAG, "HTTP client init failed");
            return false;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, body_len);

        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        const bool ok = err == ESP_OK && status >= 200 && status < 300;
        ESP_LOGI(TAG,
                 "fall upload attempt=%d status=%d err=%s ok=%s",
                 attempt,
                 status,
                 esp_err_to_name(err),
                 ok ? "yes" : "no");
        if (ok) return true;
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}
