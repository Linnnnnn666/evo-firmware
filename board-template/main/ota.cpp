// OTA 能力层：开机 + 定时检查服务器最新固件，esp_https_ota 升级，失败自动回滚
// 2026-08-24：失败重试 2 次 + 遥测上报 ota_err（服务器可远程诊断，不必抓串口）
char g_ota_err[80] = {0};   // 最后一次 OTA 失败原因（telemetry.cpp 上报）
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "cJSON.h"
#include "ota.h"
#include "board_config.h"

static const char *TAG = "ota";

static bool fw_newer(const char *remote) {
    // 版本比较 vX.Y.Z（兼容 remote 带 v / local 不带 v 的混合写法）
    int r[3] = {0}, l[3] = {0};
    int nr = sscanf(remote, "v%d.%d.%d", &r[0], &r[1], &r[2]);
    if (nr < 3) nr = sscanf(remote, "%d.%d.%d", &r[0], &r[1], &r[2]);
    int nl = sscanf(BOARD_FW_VERSION, "v%d.%d.%d", &l[0], &l[1], &l[2]);
    if (nl < 3) nl = sscanf(BOARD_FW_VERSION, "%d.%d.%d", &l[0], &l[1], &l[2]);
    if (nr < 3 || nl < 3) return true; // 解析失败保守触发一次检查
    for (int i = 0; i < 3; i++) {
        if (r[i] != l[i]) return r[i] > l[i];
    }
    return false;
}

static esp_err_t _http_get_latest(char *buf, size_t len) {
    char latest_url[160];
    snprintf(latest_url, sizeof(latest_url), "%s/firmware/%s/latest.json", BOARD_API_BASE, BOARD_DEVICE_ID);
    esp_http_client_config_t cfg = {
        .url = latest_url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int cl = esp_http_client_fetch_headers(client);
        int total = 0;
        if (cl >= 0) {
            while (total < (int)len - 1) {
                int r = esp_http_client_read(client, buf + total, len - 1 - total);
                if (r <= 0) break;
                total += r;
            }
        }
        buf[total] = 0;
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_check_now(void) {
    char body[512] = {0};
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (_http_get_latest(body, sizeof(body)) != ESP_OK) {
        ESP_LOGW(TAG, "获取版本信息失败");
        return result;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return result;
    }
    cJSON *v = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(v) && v->valuestring && fw_newer(v->valuestring)) {
        ESP_LOGW(TAG, "发现新版本 %s（当前 %s），开始 OTA", v->valuestring, BOARD_FW_VERSION);
        cJSON *bin = cJSON_GetObjectItem(root, "bin");
        char url[256];
        snprintf(url, sizeof(url), "%s/firmware/%s/%s/%s",
                 BOARD_API_BASE, BOARD_DEVICE_ID, v->valuestring,
                 cJSON_IsString(bin) && bin->valuestring ? bin->valuestring : "merged.bin");
        esp_http_client_config_t ota_cfg = {
            .url = url,
            .timeout_ms = 60000,
            .skip_cert_common_name_check = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_https_ota_config_t ota = {
            .http_config = &ota_cfg,
        };
        esp_err_t ret = esp_https_ota(&ota);
        if (ret != ESP_OK) {
            // 失败重试 2 次（网络抖动/下载中断场景）
            snprintf(g_ota_err, sizeof(g_ota_err), "ota_fail_1:%s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "OTA 第 1 次失败 %s，重试...", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(3000));
            ret = esp_https_ota(&ota);
            if (ret != ESP_OK) {
                snprintf(g_ota_err, sizeof(g_ota_err), "ota_fail_2:%s", esp_err_to_name(ret));
                ESP_LOGE(TAG, "OTA 第 2 次失败 %s，重试...", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(5000));
                ret = esp_https_ota(&ota);
                if (ret != ESP_OK) {
                    snprintf(g_ota_err, sizeof(g_ota_err), "ota_fail_3:%s", esp_err_to_name(ret));
                }
            }
        }
        if (ret == ESP_OK) {
            snprintf(g_ota_err, sizeof(g_ota_err), "ok");
            ESP_LOGI(TAG, "OTA 成功，重启...");
            esp_ota_mark_app_valid_cancel_rollback();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            result = ESP_OK;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA 失败 %s（将回滚到上一版本）", esp_err_to_name(ret));
            if (g_ota_err[0] == 0) {
                snprintf(g_ota_err, sizeof(g_ota_err), "ota_fail:%s", esp_err_to_name(ret));
            }
        }
    } else {
        ESP_LOGI(TAG, "固件已是最新（%s）", BOARD_FW_VERSION);
    }
    cJSON_Delete(root);
    return result;
}

static void ota_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(30000)); // 开机 30s 后首次检查（等 WiFi 就绪）
    for (;;) {
        ota_check_now();
        vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000)); // 每 6 小时检查
    }
}

void ota_init(void) {
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
}
