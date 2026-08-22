/* ================================================================
 * LD6002B 雷达 — TinyFrame 协议解析 + Z 轴跌倒状态机
 *
 * 使用官方 TinyFrame 状态机逐字节解析（与 TF_Demo 中 TinyFrame.c 一致）
 * 帧格式：SOF(0x01)+ID(2B BE)+LEN(2B BE)+TYPE(2B BE)+HCK(1B)+DATA(N)+DCK(1B)
 * 校验：XOR 后取反
 * ================================================================ */

#include "radar.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "radar";


// ---- 全局 ----
static float g_last_x = 0, g_last_y = 0, g_last_z = 0, g_last_speed = 0;
static int   g_target_count = 0;
static volatile bool g_triggered = false;
static int64_t g_last_alert_time = 0;  // 冷却计时
static int64_t g_posture_start_us = 0;
static int64_t g_last_event_us = 0;
static int g_high_streak = 0;
static int g_low_streak = 0;
static bool g_standing = false;

// A single radar report is deliberately not enough to trigger.  The target
// must first be high for several reports, then low for several reports within
// a short window.  This rejects isolated targets and parser/measurement noise.
static void update_fall_state(float z) {
    if (!(z > 0.05f) || z > 8.0f) return;

    const int64_t now = esp_timer_get_time();
    const int64_t max_dt_us = (int64_t)(FALL_MAX_DT_SEC * 1000000.0f);
    const int64_t cooldown_us = (int64_t)FALL_COOLDOWN_MS * 1000LL;

    if (g_standing && now - g_posture_start_us > max_dt_us) {
        g_standing = false;
        g_high_streak = 0;
        g_low_streak = 0;
    }

    if (z >= FALL_Z_HIGH) {
        ++g_high_streak;
        g_low_streak = 0;
        if (!g_standing && g_high_streak >= FALL_HIGH_STREAK_REQUIRED) {
            g_standing = true;
            g_posture_start_us = now;
            printf("[RADAR] Standing baseline acquired (z=%.2f m)\n", (double)z);
        } else if (g_standing) {
            g_posture_start_us = now;
        }
        return;
    }

    if (!g_standing) return;

    if (z <= FALL_Z_LOW) {
        ++g_low_streak;
        if (g_low_streak >= FALL_LOW_STREAK_REQUIRED) {
            const bool in_window = (now - g_posture_start_us <= max_dt_us);
            const bool out_of_cooldown = (now - g_last_event_us >= cooldown_us);
            if (in_window && out_of_cooldown) {
                g_triggered = true;
                g_last_event_us = now;
                printf("[RADAR] Suspected fall: high-to-low transition, z=%.2f m\n",
                       (double)z);
            }
            g_standing = false;
            g_high_streak = 0;
            g_low_streak = 0;
        }
    } else {
        g_low_streak = 0;
    }
}

// TinyFrame wire format is big-endian for ID/LEN/TYPE, while every numeric
// value carried in DATA is little-endian (LD6002B protocol V1.2).
static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static int32_t read_le_i32(const uint8_t *p) {
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static float read_le_float(const uint8_t *p) {
    float value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static bool checksum_ok(const uint8_t *p, size_t len, uint8_t expected) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum ^= p[i];
    return (uint8_t)~sum == expected;
}

static void handle_target_frame(uint16_t type, const uint8_t *data, uint16_t len) {
    if (len < 4) return;

    const int32_t target_num = read_le_i32(data);
    if (target_num < 0 || target_num > 20) {
        printf("[RADAR] Invalid target count: %ld\n", (long)target_num);
        return;
    }

    if (target_num == 0) {
        g_target_count = 0;
        return;
    }
    if (len < (uint16_t)(4 + target_num * 20)) {
        printf("[RADAR] Short frame: type=0x%04X len=%u targets=%ld\n",
               type, len, (long)target_num);
        return;
    }

    const uint8_t *record = data + 4;
    int32_t cluster_or_doppler;
    if (type == 0x0A04) {
        // Target report: x, y, z, dop_idx, cluster_id.
        g_last_x = read_le_float(record);
        g_last_y = read_le_float(record + 4);
        g_last_z = read_le_float(record + 8);
        cluster_or_doppler = read_le_i32(record + 12);
        g_last_speed = (float)cluster_or_doppler;
    } else {
        // Point cloud: cluster_index, x_point, y_point, z_point, speed.
        cluster_or_doppler = read_le_i32(record);
        g_last_x = read_le_float(record + 4);
        g_last_y = read_le_float(record + 8);
        g_last_z = read_le_float(record + 12);
        g_last_speed = read_le_float(record + 16);
    }
    g_target_count = target_num;
    // Only clustered target reports describe a tracked person's body
    // position.  Point-cloud samples are not used for fall decisions.
    if (type == 0x0A04) update_fall_state(g_last_z);

    printf("[RADAR] type=0x%04X targets=%d x=%.2f y=%.2f z=%.2f metric=%ld\n",
           type, g_target_count, (double)g_last_x, (double)g_last_y,
           (double)g_last_z, (long)cluster_or_doppler);
}

static void parse_tf_stream(uint8_t *stream, int *stream_len) {
    while (*stream_len > 0) {
        int start = 0;
        while (start < *stream_len && stream[start] != TF_SOF) ++start;
        if (start == *stream_len) { *stream_len = 0; return; }
        if (start > 0) {
            memmove(stream, stream + start, *stream_len - start);
            *stream_len -= start;
        }
        if (*stream_len < 8) return; // SOF + ID + LEN + TYPE + header checksum

        const uint16_t payload_len = read_be16(stream + 3);
        const int frame_len = 9 + payload_len;
        if (payload_len > TF_MAX_PAYLOAD) { // Bad SOF; resynchronise on the next byte.
            memmove(stream, stream + 1, --*stream_len);
            continue;
        }
        if (*stream_len < frame_len) return;

        const bool header_valid = checksum_ok(stream, 7, stream[7]);
        const bool data_valid = checksum_ok(stream + 8, payload_len, stream[8 + payload_len]);
        if (header_valid && data_valid) {
            const uint16_t type = read_be16(stream + 5);
            if (type == 0x0A04 || type == TF_TYPE_POINTCLOUD)
                handle_target_frame(type, stream + 8, payload_len);
        } else {
            printf("[RADAR] Bad TF checksum\n");
        }
        memmove(stream, stream + frame_len, *stream_len - frame_len);
        *stream_len -= frame_len;
    }
}

// ---- TF 发送 ----
static uint16_t g_tf_id = 0;

static void tf_send(uint16_t type, uint8_t *data, uint16_t len) {
    // The host is the TinyFrame master: its peer bit (ID bit 15) must be 1.
    // The remaining 15 bits monotonically identify successive commands.
    uint16_t id = (g_tf_id++ & 0x7FFF) | 0x8000;

    uint8_t buf[256];
    int p = 0;

    buf[p++] = 0x01;                    // SOF
    buf[p++] = (id >> 8) & 0xFF;        // ID hi
    buf[p++] = id & 0xFF;               // ID lo
    buf[p++] = (len >> 8) & 0xFF;       // LEN hi
    buf[p++] = len & 0xFF;              // LEN lo
    buf[p++] = (type >> 8) & 0xFF;      // TYPE hi
    buf[p++] = type & 0xFF;             // TYPE lo

    // HEAD_CKSUM
    uint8_t hck = 0;
    for (int i = 0; i < p; i++) hck ^= buf[i];
    hck = ~hck;
    buf[p++] = hck;

    // DATA + DATA_CKSUM
    if (len > 0 && data != NULL) {
        uint8_t dck = 0;
        for (int i = 0; i < len; i++) {
            buf[p] = data[i];
            dck ^= data[i];
            p++;
        }
        dck = ~dck;
        buf[p++] = dck;
    }

    uart_write_bytes(RADAR_UART_PORT, buf, p);

    // 打印 hex
    printf("[RADAR] TX: ");
    for (int i = 0; i < p && i < 20; i++) printf("%02X ", buf[i]);
    printf("(id=0x%04X type=0x%04X len=%d)\n", id, type, len);
}

// 0x0201 commands are 4-byte little-endian int32 values.
static void radar_send_command(uint8_t command) {
    uint8_t cmd[4] = {command, 0x00, 0x00, 0x00};
    tf_send(0x0201, cmd, sizeof(cmd));
}

static void radar_enable_pointcloud(void) {
    radar_send_command(0x06);
}

// ---- 初始化 ----
void radar_init(void) {
#if !RADAR_TESTBOARD_RX_ONLY
    // A sleeping LD6002B can be woken by pulling its RX0 low before sending
    // a configuration frame (protocol V1.2, §1.3). GPIO1 is ESP TX -> radar
    // RX, so drive it low briefly before handing it back to UART1.
    gpio_config_t wake_pin = {
        .pin_bit_mask = (1ULL << RADAR_TX_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&wake_pin);
    gpio_set_level(RADAR_TX_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

    uart_config_t cfg = {
        .baud_rate  = RADAR_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(RADAR_UART_PORT, &cfg);
    uart_set_pin(RADAR_UART_PORT,
#if RADAR_TESTBOARD_RX_ONLY
                 UART_PIN_NO_CHANGE,
#else
                 RADAR_TX_PIN,
#endif
                 RADAR_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(RADAR_UART_PORT, RADAR_BUF_SIZE, 0, 0, NULL, 0);
    printf("[RADAR] UART1 ready @115200\n");
    printf("[RADAR] Fall mode=%s z_high=%.2f z_low=%.2f high_frames=%d low_frames=%d window=%.1fs\n",
           RADAR_DEMO_MODE ? "DEMO_EASY" : "STRICT",
           (double)FALL_Z_HIGH, (double)FALL_Z_LOW,
           FALL_HIGH_STREAK_REQUIRED, FALL_LOW_STREAK_REQUIRED,
           (double)FALL_MAX_DT_SEC);

    // ★ LD6002B V3.9 需要收到命令才会上报数据
    // 先等雷达充分启动（500ms），再发命令
 #if !RADAR_TESTBOARD_RX_ONLY
    vTaskDelay(pdMS_TO_TICKS(500));

    // Match the validated upper-computer configuration: ceiling mount,
    // high sensitivity, fast trigger, and target + point-cloud reports.
    // Each command is 0x0201 with a 4-byte little-endian command payload.
    radar_send_command(0x13); // top/ceiling installation
    vTaskDelay(pdMS_TO_TICKS(30));
    radar_send_command(0x0C); // high sensitivity
    vTaskDelay(pdMS_TO_TICKS(30));
    radar_send_command(0x10); // fast trigger speed
    vTaskDelay(pdMS_TO_TICKS(30));
    radar_send_command(0x08); // enable target reports
    vTaskDelay(pdMS_TO_TICKS(30));
    // 0x06 = enable point-cloud reports
    radar_enable_pointcloud();
    printf("[RADAR] Applied top/high/fast + target/pointcloud configuration\n");
#else
    printf("[RADAR] Test-board RX-only mode: listening on GPIO2\n");
#endif

    vTaskDelay(pdMS_TO_TICKS(500));
    printf("[RADAR] Ready — waiting for data...\n");
}

// ---- TF frame receiver: 0x0A04 target reports + 0x0A08 point-cloud reports ----
void radar_task(void *arg) {
    printf("[RADAR] Task started — TF frame parser\n");

    uint8_t  stream[TF_MAX_PAYLOAD + 9];
    int      stream_len = 0;
    int64_t  last_wakeup = esp_timer_get_time();
    int64_t  last_rx_time = last_wakeup;

    while (1) {
        uint8_t buf[256];
        int rd = uart_read_bytes(RADAR_UART_PORT, buf, sizeof(buf),
                                 pdMS_TO_TICKS(100));
        if (rd > 0) {
            last_rx_time = esp_timer_get_time();
            if (rd > (int)sizeof(stream) - stream_len) {
                // Keep the newest bytes if malformed traffic overflows the parser buffer.
                int keep = sizeof(stream) - rd;
                if (keep > 0) {
                    memmove(stream, stream + stream_len - keep, keep);
                    stream_len = keep;
                } else {
                    stream_len = 0;
                }
            }
            memcpy(stream + stream_len, buf, rd);
            stream_len += rd;
            parse_tf_stream(stream, &stream_len);
        }

        int64_t now = esp_timer_get_time();
        // 手册说明：配置后没有回包时，重新下发配置消息可将模块从
        // 无人低功耗模式唤醒。无数据期间每 2 秒重试一次。
#if !RADAR_TESTBOARD_RX_ONLY
        if (now - last_rx_time >= 2000000LL && now - last_wakeup >= 2000000LL) {
            printf("[RADAR] No response, retry enable pointcloud\n");
            radar_enable_pointcloud();
            last_wakeup = now;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- 供 main.cpp 调用 ----
bool radar_is_triggered(void) { return g_triggered; }
void radar_clear_trigger(void) { g_triggered = false; }
void radar_mark_confirmed(void) {
    g_last_alert_time = esp_timer_get_time();
    g_triggered = false;
}
int radar_get_target_count(void) { return g_target_count; }
void radar_get_last_target(float *x, float *y, float *z, float *speed) {
    *x = g_last_x; *y = g_last_y; *z = g_last_z; *speed = g_last_speed;
}
