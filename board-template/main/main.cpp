// 通用引导固件业务模板：LED 指示 + 业务遥测字段（board_extra_json）
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_config.h"
#include "network.h"
#include "ota.h"
#include "telemetry.h"

#ifdef BOARD_LED_WS2812
// 板载 RGB LED（WS2812 可寻址灯珠）：上电后常亮红色；BOARD_LED_DEFAULT_OFF 板默认熄灭
#include "driver/rmt_tx.h"

#define WS2812_RMT_RESOLUTION_HZ 10000000  /* 10 MHz：1 tick = 0.1 us */
#define WS2812_T0H_TICKS  3                /* 0 码：0.3 us 高 + 0.9 us 低 */
#define WS2812_T0L_TICKS  9
#define WS2812_T1H_TICKS  9                /* 1 码：0.9 us 高 + 0.3 us 低 */
#define WS2812_T1L_TICKS  3
#define WS2812_RESET_TICKS 600             /* 帧尾复位：60 us 低电平（>50 us 触发锁存） */

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

static void ws2812_fill_symbol(rmt_symbol_word_t *sym, uint16_t high_ticks, uint16_t low_ticks) {
    sym->duration0 = high_ticks;
    sym->level0 = 1;
    sym->duration1 = low_ticks;
    sym->level1 = 0;
}

// 单颗灯珠设置颜色（字节流 G,R,B 顺序发出，MSB 先发）；static 缓冲保证 RMT 异步拷贝安全
static void ws2812_set_color(uint8_t g, uint8_t r, uint8_t b) {
    const uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    static rmt_symbol_word_t syms[24 + 1];
    for (int i = 23; i >= 0; i--) {
        if (grb & (1u << i))
            ws2812_fill_symbol(&syms[23 - i], WS2812_T1H_TICKS, WS2812_T1L_TICKS);
        else
            ws2812_fill_symbol(&syms[23 - i], WS2812_T0H_TICKS, WS2812_T0L_TICKS);
    }
    syms[24].duration0 = WS2812_RESET_TICKS;
    syms[24].level0 = 0;
    syms[24].duration1 = 0;
    syms[24].level1 = 0;
    // ESP-IDF 5.x：config 必须非空（传 NULL 会 invalid argument）
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, syms, sizeof(syms), &tx_cfg);
}

static void ws2812_init(void) {
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)BOARD_LED_GPIO;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = WS2812_RMT_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_led_chan));

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &s_led_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_led_chan));
#ifdef BOARD_LED_DEFAULT_OFF
    // 默认熄灭板（如 board-s3-5798）：上电不点亮，等待 MQTT led_on 指令
    ws2812_set_color(0, 0, 0);
#elif defined(BOARD_LED_COLOR_GRB)
    // 板配置可指定上电颜色（如 board-s3-477c：蓝色常亮）
    ws2812_set_color(BOARD_LED_COLOR_GRB);
#else
    // 默认红色常亮（v0.4.0：由绿色常亮改为红色常亮，GRB 标准顺序：r=255, g=0, b=0）
    ws2812_set_color(0, 255, 0);
#endif
}
#endif

static const char *TAG = "board";

// 业务扩展遥测字段（拼进 MQTT JSON：,"led":1,"pwm_duty":128）；LED 开关状态由 board_led_set 动态刷新
#ifdef BOARD_LED_DEFAULT_OFF
const char *board_extra_json = ",\"led\":0";  /* 默认熄灭板：启动即上报 led=0 */
#else
const char *board_extra_json = ",\"led\":1";
#endif

#ifdef BOARD_LED_WS2812
// 呼吸灯状态（单色呼吸分支使用）：1 = 呼吸亮起，0 = 熄灭（黑色）
static volatile int s_breath_on = 1;
// 当前实际驱动亮度（0-255）：呼吸循环每步写入，board_led_set/遥测读取，真实硬件驱动值
static volatile uint8_t s_breath_duty = 0;

#if !defined(BOARD_LED_CYCLE_RGB) && !defined(BOARD_LED_COLOR_GRB)
// 呼吸灯板：led = 实际亮灭，pwm_duty = 当前实际驱动亮度（0-255 渐变值）
static char s_led_extra[48];
static void led_extra_update(int on, int duty) {
    snprintf(s_led_extra, sizeof(s_led_extra), ",\"led\":%d,\"pwm_duty\":%d", on, duty);
    board_extra_json = s_led_extra;
}
#else
// 常亮/循环呼吸板：保持原有 led 字段格式不变
static void led_extra_update(int on, int duty) {
    (void)duty;
    board_extra_json = on ? ",\"led\":1" : ",\"led\":0";
}
#endif

// 常亮颜色模式（board_led_set_color 指令，如 led_green_on）：
// s_solid_on=1 时呼吸循环持续按 s_solid_grb 满亮度常亮，不被呼吸/熄灯逻辑覆盖
static volatile int s_solid_on = 0;
static volatile uint8_t s_solid_grb[3] = {0, 0, 0};

// 常亮模式遥测：如实上报 亮灭 + 实际驱动颜色（#RRGGBB，即写入 WS2812 的 RGB 值）+ 满亮度
static char s_solid_extra[64];
static void solid_extra_update(void) {
    if (s_solid_on) {
        snprintf(s_solid_extra, sizeof(s_solid_extra),
                 ",\"led\":1,\"led_color\":\"#%02X%02X%02X\",\"pwm_duty\":255",
                 s_solid_grb[1], s_solid_grb[0], s_solid_grb[2]);
    } else {
        snprintf(s_solid_extra, sizeof(s_solid_extra), ",\"led\":0");
    }
    board_extra_json = s_solid_extra;
}
#endif

// 收到过远程开关指令后，普通 GPIO LED 分支停止本地自动翻转，完全听命于遥控
static volatile int s_led_remote_ctrl = 0;

// 定时自动关灯（仅定义了 BOARD_LED_AUTO_OFF_S 的板启用，如 board-s3-5798）：
// 收到 led_on 点亮后，BOARD_LED_AUTO_OFF_S 秒（60s）自动熄灭，无需再发 led_off
#ifdef BOARD_LED_AUTO_OFF_S
static void led_auto_off_arm(void);
static void led_auto_off_cancel(void);
#endif

// MQTT 远程开关板载 LED（telemetry.cpp 收到 led_on/led_off 指令后调用）
// WS2812：亮 = 按板配置颜色点亮，灭 = 全黑；普通 GPIO LED：直接拉高/拉低
extern "C" void board_led_set(int on) {
    s_led_remote_ctrl = 1;
#ifdef BOARD_LED_AUTO_OFF_S
    if (on) {
        led_auto_off_arm();      // led_on：60 秒后自动熄灭
    } else {
        led_auto_off_cancel();   // 手动 led_off：取消未到期的自动关灯
    }
#endif
    if (on) {
#ifdef BOARD_LED_WS2812
        led_extra_update(1, (int)s_breath_duty);
#else
        board_extra_json = ",\"led\":1";
#endif
    } else {
#ifdef BOARD_LED_WS2812
        led_extra_update(0, 0);
#else
        board_extra_json = ",\"led\":0";
#endif
    }
#ifdef BOARD_LED_WS2812
    s_solid_on = 0;   /* 普通 led_on/led_off 退出常亮颜色模式（回到呼吸/熄灭） */
    if (on) {
        s_breath_on = 1;   /* 呼吸灯板：led_on = 恢复呼吸（颜色/亮度由呼吸循环实时驱动） */
    } else {
        s_breath_on = 0;
    }
    if (s_led_chan == NULL) {
        return;  // RMT 通道尚未初始化（指令早于 led_task 启动时兜底）
    }
    if (on) {
#ifdef BOARD_LED_COLOR_GRB
        ws2812_set_color(BOARD_LED_COLOR_GRB);
#endif
    } else {
        ws2812_set_color(0, 0, 0);
    }
#else
    gpio_set_level((gpio_num_t)BOARD_LED_GPIO, on ? 1 : 0);
#endif
}

// MQTT 颜色常亮指令（如 led_green_on）：点亮为指定颜色并保持常亮（不呼吸、不自动关灯）
// WS2812：立即写入目标颜色，呼吸循环持续保持；普通 GPIO LED：直接拉高
extern "C" void board_led_set_color(uint8_t g, uint8_t r, uint8_t b) {
    s_led_remote_ctrl = 1;
#ifdef BOARD_LED_AUTO_OFF_S
    led_auto_off_cancel();   /* “保持常亮”：常亮颜色模式不参与 led_on 的 60 秒自动关灯 */
#endif
#ifdef BOARD_LED_WS2812
    s_solid_on = 1;
    s_solid_grb[0] = g;
    s_solid_grb[1] = r;
    s_solid_grb[2] = b;
    s_breath_on = 0;         /* 退出呼吸，进入常亮 */
    if (s_led_chan != NULL) {
        ws2812_set_color(g, r, b);   /* 立即点亮目标颜色 */
    }
    solid_extra_update();    /* 遥测：led=1 + 实际驱动颜色 + 满亮度 */
#else
    gpio_set_level((gpio_num_t)BOARD_LED_GPIO, 1);
    board_extra_json = ",\"led\":1";
#endif
}

#ifdef BOARD_LED_AUTO_OFF_S
static esp_timer_handle_t s_led_auto_off_timer = NULL;

// 定时到：自动熄灭（走 board_led_set(0) 同一路径，硬件关灯 + 遥测 led=0 同步刷新）
static void led_auto_off_cb(void *arg) {
    ESP_LOGI(TAG, "led_on 已满 %d 秒，自动关灯", BOARD_LED_AUTO_OFF_S);
    board_led_set(0);
}

// 武装自动关灯：重复 led_on 会重新计时；定时器创建失败仅告警，不影响本次点亮
static void led_auto_off_arm(void) {
    if (s_led_auto_off_timer == NULL) {
        esp_timer_create_args_t args = {
            .callback = led_auto_off_cb,
            .arg = NULL,
            .name = "led_auto_off",
        };
        if (esp_timer_create(&args, &s_led_auto_off_timer) != ESP_OK) {
            ESP_LOGE(TAG, "自动关灯定时器创建失败");
            return;
        }
    }
    esp_timer_stop(s_led_auto_off_timer);  // 已运行则先停，确保重新计时
    esp_timer_start_once(s_led_auto_off_timer, (uint64_t)BOARD_LED_AUTO_OFF_S * 1000000ULL);
}

// 取消未到期的自动关灯（定时器未运行/已触发时报错可忽略）
static void led_auto_off_cancel(void) {
    if (s_led_auto_off_timer != NULL) {
        esp_timer_stop(s_led_auto_off_timer);
    }
}
#endif

// 单色呼吸灯峰值颜色（G,R,B）：默认纯蓝色；板配置可覆盖（如 board-s3-477c 类板）
// 三色循环渐变呼吸（BOARD_LED_CYCLE_RGB）与常亮（BOARD_LED_COLOR_GRB）不需要该配置，
// 避免未使用告警故加条件编译
#if !defined(BOARD_LED_CYCLE_RGB) && !defined(BOARD_LED_COLOR_GRB)
#ifndef BOARD_LED_BREATH_GRB
#define BOARD_LED_BREATH_GRB 0, 0, 255
#endif
static const uint8_t s_breath_grb[3] = { BOARD_LED_BREATH_GRB };
#endif

static void led_task(void *arg) {
#ifdef BOARD_LED_WS2812
    // 板载 RGB LED（WS2812，GPIO 48）
    // WS2812 为可寻址灯珠：用每帧 RGB 值按亮度缩放实现等效 PWM 调光（占空比 0%→100%→0%）
    ws2812_init();
#ifdef BOARD_LED_DEFAULT_OFF
    board_extra_json = ",\"led\":0";  /* 默认熄灭：上电 LED 不亮，等 MQTT led_on */
#else
    board_extra_json = ",\"led\":1";
#endif

#ifdef BOARD_LED_COLOR_GRB
    // 板配置指定颜色：常亮（如 board-s3-477c：蓝色常亮）；
    // BOARD_LED_DEFAULT_OFF 板（board-s3-5798）：ws2812_init() 已置黑熄灭，此处保持即可
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#elif defined(BOARD_LED_CYCLE_RGB)
    // 三色循环渐变呼吸灯：红→蓝→绿→红，颜色在整周期内分段线性插值平滑过渡；
    // 整体周期约 3.6s（180 步 × 20ms），亮度 0→峰值→0（暗→亮→暗）
    static const uint8_t s_cycle_grb[4][3] = {
        {0, 255, 0},   /* 红 (G,R,B) */
        {0, 0, 255},   /* 蓝 */
        {255, 0, 0},   /* 绿 */
        {0, 255, 0},   /* 红（回到起点，闭合循环） */
    };
    const int steps = 180;    /* 每循环 180 步 */
    const int step_ms = 20;   /* 步进 20ms → 周期约 3.6s（3~4s 范围内） */
    for (;;) {
        for (int i = 0; i < steps; i++) {
            /* 正弦亮度：i=0 → 0，i=steps/2 → 峰值，i=steps → 0 */
            double phase = 2.0 * 3.14159265358979323846 * i / steps;
            uint8_t v = (uint8_t)(127.5 * (1.0 - cos(phase)));
            /* 颜色：0~3 映射到 4 个关键色点，在红→蓝、蓝→绿、绿→红三段内线性插值 */
            double t = 3.0 * i / steps;
            int seg = (int)t;              /* 0/1/2 段（i<steps 时 t<3，seg+1 最大为 3，安全） */
            double u = t - seg;            /* 段内进度 0~1 */
            uint8_t g = (uint8_t)(s_cycle_grb[seg][0] * (1.0 - u) + s_cycle_grb[seg + 1][0] * u);
            uint8_t r = (uint8_t)(s_cycle_grb[seg][1] * (1.0 - u) + s_cycle_grb[seg + 1][1] * u);
            uint8_t b = (uint8_t)(s_cycle_grb[seg][2] * (1.0 - u) + s_cycle_grb[seg + 1][2] * u);
            ws2812_set_color((uint8_t)((uint32_t)g * v / 255),
                             (uint8_t)((uint32_t)r * v / 255),
                             (uint8_t)((uint32_t)b * v / 255));
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }
#else
    // 单色呼吸灯（默认蓝色，板可配置颜色）：每周期约 2s（亮度 0→最大→0）
    const int steps = 100;    /* 每周期 100 步 */
    const int step_ms = 20;   /* 步进 20ms → 周期约 2.0s */
    int i = 0;
    for (;;) {
        uint8_t v = 0;
        if (s_solid_on) {
            /* 常亮颜色模式（led_green_on 等指令）：满亮度常亮指定颜色，呼吸循环不覆盖 */
            s_breath_duty = 255;
            ws2812_set_color(s_solid_grb[0], s_solid_grb[1], s_solid_grb[2]);
            solid_extra_update();
            vTaskDelay(pdMS_TO_TICKS(step_ms));
            continue;
        }
        if (s_breath_on) {
            /* 正弦亮度：i=0 → 0，i=steps/2 → 峰值，i=steps → 0；按板配置颜色缩放 */
            double phase = 2.0 * 3.14159265358979323846 * i / steps;
            v = (uint8_t)(127.5 * (1.0 - cos(phase)));
            i = (i + 1) % steps;
        } else {
            i = 0;  /* 熄灭时相位归零：下次 led_on 从暗处平滑亮起 */
        }
        s_breath_duty = v;  /* 记录真实驱动亮度，供遥测/led_on 上报 */
        ws2812_set_color((uint8_t)(s_breath_grb[0] * v / 255),
                         (uint8_t)(s_breath_grb[1] * v / 255),
                         (uint8_t)(s_breath_grb[2] * v / 255));
        led_extra_update(s_breath_on ? 1 : 0, v);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
#endif
#else
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    // 启动就绪提示：闪烁 3 次（每次亮 200ms / 灭 200ms）
    for (int i = 0; i < 3; i++) {
        gpio_set_level((gpio_num_t)BOARD_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level((gpio_num_t)BOARD_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int state = 0;
    for (;;) {
        if (!s_led_remote_ctrl) {
            state = !state;
            gpio_set_level((gpio_num_t)BOARD_LED_GPIO, state);
            board_extra_json = state ? ",\"led\":1" : ",\"led\":0";
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "board %s (%s) starting, fw %s", BOARD_DEVICE_ID, BOARD_MODEL, BOARD_FW_VERSION);
    network_init();
    ota_init();
    telemetry_init();
    xTaskCreate(led_task, "led_task", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "board %s ready", BOARD_DEVICE_ID);
}
