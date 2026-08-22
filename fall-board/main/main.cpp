/* ===================================================================
 * 纯 TensorFlow Lite Micro 推理 — 零行 Edge Impulse SDK 代码
 *
 * 和 Edge Impulse SDK 版的区别：
 *   - 不引入 edge-impulse-sdk/ 的任何文件（省掉 2000+ 个源文件）
 *   - 不调用 run_classifier()，手动走完"加载→注册→分配→推理→反量化"全流程
 *   - 预处理自己写（RGB→灰度→缩放→INT8），不依赖 SDK 的 DSP 模块
 *
 * 依赖（由 idf_component.yml 自动从乐鑫组件注册表下载）：
 *   espressif/esp-tflite-micro  — Google TFLite Micro + ESP-NN 加速
 *   espressif/esp32-camera      — OV2640 摄像头驱动
 *
 * 模型：main/model/model.h，由 Python 脚本从 .tflite 生成
 *       是一个 300328 字节的 const unsigned char 数组
 * =================================================================== */

/* ===== 系统头文件 ===== */
#include <stdio.h>      // printf, ESP_LOGI
#include <stdlib.h>     // malloc, free
#include "freertos/FreeRTOS.h"   // FreeRTOS 内核（vTaskDelay、任务调度）
#include "freertos/task.h"       // pdMS_TO_TICKS（毫秒 → tick 数）
#include "driver/gpio.h"         // gpio_config_t、gpio_get_level（按键）
#include "esp_system.h"          // ESP-IDF 系统基础
#include "esp_pm.h"              // 动态 CPU 频率与性能锁
#include "esp_log.h"             // ESP_LOGE、ESP_LOGI 日志宏
#include "esp_camera.h"          // esp_camera_init、esp_camera_fb_get（摄像头）
#include "esp_task_wdt.h"        // 看门狗：esp_task_wdt_add、esp_task_wdt_reset
#include "led_strip.h"           // 板载 WS2812 RGB 灯（espressif/led_strip）
#include "radar.h"               // LD6002B 雷达协议解析 + 跌倒状态机
#include "network.h"
#include "ota.h"
#include "telemetry.h"             // Wi-Fi + 本地电脑 HTTP 上报

/* ===== 纯 TFLite Micro 头文件（来自 managed_components/espressif__esp-tflite-micro）===== */
#include "tensorflow/lite/micro/micro_interpreter.h"         // MicroInterpreter 类
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h" // 算子注册
#include "tensorflow/lite/micro/system_setup.h"              // InitializeTarget()
#include "tensorflow/lite/schema/schema_generated.h"         // tflite::GetModel()

/* ===== 模型（Python 脚本自动生成，纯 C 数组，零 SDK 依赖）===== */
#include "model/model.h"
// 这个头文件里只有两行有效内容：
//   alignas(16) static const unsigned char g_model[] = { 0x1c, 0x00, ... };
//   static const unsigned int g_model_len = 300328;
// g_model 存在 Flash 的 .rodata 段，TFLite Micro 通过 GetModel() 指针直接解析


// ===== 按键引脚 =====
#define BTN_GPIO          GPIO_NUM_42    // 你接的按键所在 IO 口
#define BTN_ACTIVE_LEVEL  0              // 按下 = 低电平（GND）

// ===== 板载 RGB 灯（WS2812 可寻址 LED）=====
// ESP32-S3-EYE 板载 WS2812 接在 GPIO 48；如硬件不同改这里即可
#define RGB_LED_GPIO      GPIO_NUM_48

// ===== ESP32-S3-EYE 摄像头引脚（OV2640 → ESP32-S3 的 8 位并行接口）=====
// ⚠️ 如果换板子（如 AI-Thinker ESP32-CAM），这里要全部替换
#define PWDN_GPIO_NUM  -1       // 断电引脚，-1 = 不用
#define RESET_GPIO_NUM -1       // 复位引脚，-1 = 不用
#define XCLK_GPIO_NUM  15       // 主时钟（20MHz）
#define SIOD_GPIO_NUM   4       // SCCB 数据（类似 I2C SDA）
#define SIOC_GPIO_NUM   5       // SCCB 时钟（类似 I2C SCL）
#define Y9_GPIO_NUM    16       // D7
#define Y8_GPIO_NUM    17       // D6
#define Y7_GPIO_NUM    18       // D5
#define Y6_GPIO_NUM    12       // D4
#define Y5_GPIO_NUM    10       // D3
#define Y4_GPIO_NUM     8       // D2
#define Y3_GPIO_NUM     9       // D1
#define Y2_GPIO_NUM    11       // D0
#define VSYNC_GPIO_NUM  6       // 帧同步
#define HREF_GPIO_NUM   7       // 行同步
#define PCLK_GPIO_NUM  13       // 像素时钟

#define CAPTURE_COUNT   3        // 每次按键拍 3 帧

static const char *TAG = "pure_tflm";  // 日志标签

// Dynamic frequency scaling:
//   idle/radar monitoring: 80 MHz
//   TFLM Invoke():         240 MHz while this lock is held
static esp_pm_lock_handle_t g_inference_pm_lock = nullptr;

static bool power_management_init(void) {
#if CONFIG_PM_ENABLE
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        // Keep light sleep disabled for now: the radar UART and USB console
        // must remain responsive. Frequency scaling alone is deterministic.
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed: 0x%x", (unsigned)err);
        return false;
    }

    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "tflm_inference",
                             &g_inference_pm_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_lock_create failed: 0x%x", (unsigned)err);
        g_inference_pm_lock = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Power management ready: idle=80MHz, inference=240MHz");
    return true;
#else
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE is disabled; CPU frequency stays fixed");
    return false;
#endif
}

static void inference_pm_acquire(void) {
    if (g_inference_pm_lock != nullptr) {
        if (esp_pm_lock_acquire(g_inference_pm_lock) == ESP_OK) {
            ESP_LOGI(TAG, "CPU boost -> 240MHz");
        }
    }
}

static void inference_pm_release(void) {
    if (g_inference_pm_lock != nullptr) {
        if (esp_pm_lock_release(g_inference_pm_lock) == ESP_OK) {
            ESP_LOGI(TAG, "CPU idle floor -> 80MHz");
        }
    }
}


// ===== 摄像头配置（esp_camera 驱动需要的数据结构）=====
// 配置结构体 camera_config_t 在 esp_camera.h 中定义
// ⚠️ 必须用 CAMERA_FB_IN_PSRAM——帧缓冲 225KB，内部 SRAM 放不下
static camera_config_t camera_config = {
    .pin_pwdn      = PWDN_GPIO_NUM,    .pin_reset    = RESET_GPIO_NUM,
    .pin_xclk      = XCLK_GPIO_NUM,    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl  = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM, .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM, .pin_d3 = Y5_GPIO_NUM, .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM, .pin_href  = HREF_GPIO_NUM,
    .pin_pclk  = PCLK_GPIO_NUM,

    .xclk_freq_hz = 20000000,     // XCLK 20MHz（OV2640 支持 10-20MHz）
    .ledc_timer   = LEDC_TIMER_0, // 用 LEDC 硬件生成时钟
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,  // OV2640 输出 JPEG 压缩帧
    .frame_size   = FRAMESIZE_QVGA,  // 320×240（不要设太大，JPEG 质量已够）
    .jpeg_quality = 12,              // 0-63，越小质量越高
    .fb_count     = 1,               // 帧缓冲个数（≥1）
    .fb_location  = CAMERA_FB_IN_PSRAM, // ★ 关键：帧缓冲存在外部 8MB PSRAM 里
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,  // 有空缓冲就抓
};


/* ================================================================
 * 按键初始化 — 配置 GPIO 42 为输入上拉
 *
 * 上拉电阻的作用：不按按键时 IO 口被内部电阻拉到高电平。
 * 按下按键时 IO 口直接接地变低电平。不需要外接上拉电阻。
 * ================================================================ */
static void button_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),  // 只有 GPIO 42 这一个引脚
        .mode         = GPIO_MODE_INPUT,       // 输入模式
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // 启用内部上拉（不按时 = HIGH）
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // 下拉不用（不需要）
        .intr_type    = GPIO_INTR_DISABLE,     // 不注册中断，用轮询方式读
    };
    gpio_config(&cfg);
}


/* ================================================================
 * 等待按键按下 — 轮询 + 消抖
 *
 * 每 50ms 读取一次 IO 电平。检测到低电平后再等 80ms 确认不是抖动。
 * 循环里加 esp_task_wdt_reset() 喂狗——因为阻塞时间可能很长。
 * ================================================================ */
/* button_wait_press() — 保留但不使用，雷达版用轮询方式替代了阻塞等待 */


/* ================================================================
 * 摄像头初始化 — 按键按下时才调用
 *
 * esp_camera_init() 会通过 SCCB 总线配置 OV2640 的寄存器。
 * 初始化完成后做一次垂直和水平翻转（把倒像正过来）。
 * ================================================================ */
static bool camera_init(void) {
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera fail: 0x%x", err);
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();  // 获取 sensor 对象
    s->set_vflip(s, 1);     // 垂直翻转
    s->set_hmirror(s, 1);   // 水平镜像
    return true;
}


/* ================================================================
 * 摄像头反初始化 — 拍完就关，省电
 *
 * esp_camera_deinit() 释放 SCCB 总线和 DMA 通道，
 * OV2640 进入待机模式（功耗从 ~50mA 降到 <1mA）。
 * ================================================================ */
static void camera_deinit(void) {
    esp_camera_deinit();
}


/* ================================================================
 * 摄像头抓帧 — 从 OV2640 取一帧 JPEG，解码成 RGB888
 *
 * esp_camera_fb_get() 从 DMA 缓冲里取出一帧。
 * fmt2rgb888() 把 JPEG → RGB888（每个像素 3 字节）。
 * esp_camera_fb_return() 把帧缓冲归还给驱动（否则后续抓不到帧）。
 * ================================================================ */
static bool camera_capture(uint8_t *buf_320x240x3) {
    camera_fb_t *fb = esp_camera_fb_get();  // 取一帧
    if (!fb) return false;

    // JPEG 解码 → RGB888（buf 的空间由调用者预先分配）
    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, buf_320x240x3);

    esp_camera_fb_return(fb);  // ★必须归还，否则下次 esp_camera_fb_get 会失败
    return ok;
}


/* ================================================================
 * 图像预处理：RGB888 → 96×96 灰度 → INT8
 *
 * 这是 Edge Impulse SDK 里 image.hpp 的 crop_and_interpolate + 灰度化
 * 的手写等价实现。不依赖任何第三方库。
 *
 * 步骤：
 *   1. 最近邻采样：320×240 → 96×96
 *      - 比例因子 sx = 320/96 ≈ 3.33, sy = 240/96 = 2.5
 *      - 目标图坐标 (x, y) 对应原图 (x*sx, y*sy)
 *   2. BT.601 灰度化：Gray = 0.299R + 0.587G + 0.114B
 *      - 这是标准电视信号灰度公式，人眼对绿色最敏感所以权重最高
 *   3. 量化到 INT8 [-128, 127]
 *      - 模型训练时数据归一化到 [-1, 1]，对应 INT8 的 [-128, 127]
 *      - gray 范围 [0, 255]，减 128 后变 [-128, 127]
 *
 * 为什么用"最近邻"而不是"双线性插值"？
 *   模型训练时 Edge Impulse 默认用 Fit Shortest 模式（等价于最近邻）。
 *   如果训练时选了双线性，部署也要用双线性，否则精度会下降。
 *   嵌入式场景下最近邻速度快 3-4 倍，精度损失可忽略。
 * ================================================================ */
static void preprocess_rgb_to_int8(const uint8_t *rgb,
                                   int src_w, int src_h,
                                   int8_t *dst, int dst_w, int dst_h) {
    float sx = (float)src_w / dst_w;  // 水平缩放因子
    float sy = (float)src_h / dst_h;  // 垂直缩放因子

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            // 最近邻：目标坐标 → 原图坐标（取整）
            int sx_i = (int)(x * sx);
            int sy_i = (int)(y * sy);

            // 原图一维索引（RGB888，每个像素 3 字节）
            int idx  = (sy_i * src_w + sx_i) * 3;

            // BT.601 灰度公式
            // rgb[idx]   = B（蓝）
            // rgb[idx+1] = G（绿）
            // rgb[idx+2] = R（红）
            // 注意：esp32-camera 的 JPEG 解码输出是 BGR 序！
            float gray = 0.299f * rgb[idx + 2]   // R 权重 0.299
                       + 0.587f * rgb[idx + 1]   // G 权重 0.587
                       + 0.114f * rgb[idx];       // B 权重 0.114

            // 浮点灰度 [0, 255] → INT8 [-128, 127]
            dst[y * dst_w + x] = (int8_t)(gray - 128);
        }
    }
}


/* ================================================================
 * 纯 TFLite Micro 推理引擎 — 全局变量（只初始化一次）
 *
 * 为什么用 static 全局变量而不是函数内局部变量？
 *   g_tensor_arena 有 128KB，函数栈只有 4KB（FreeRTOS 默认），放不下。
 *   static 让这些变量在 BSS 段分配（启动时清零），生命周期 = 程序运行期。
 *
 * __attribute__((aligned(16)))：TFLite Micro 要求 arena 16 字节对齐，
 *   否则某些 ESP-NN SIMD 指令会触发 alignment fault。
 *
 * MicroMutableOpResolver<5>：<5> = 模型需要的算子个数。
 *   如果模型多加了一个算子（如 Relu），<5> 要改成 <6>，
 *   否则 AddRelu() 会运行时崩溃。
 * ================================================================ */
static tflite::MicroMutableOpResolver<5> g_resolver;
static uint8_t g_tensor_arena[128 * 1024] __attribute__((aligned(16)));
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input  = nullptr;   // 模型输入（96×96 INT8）
static TfLiteTensor *g_output = nullptr;   // 模型输出（2 个 INT8 → fall, normal）


/* ================================================================
 * 推理引擎初始化 — 芯片启动时执行一次
 *
 * 这 5 步就是 Edge Impulse run_classifier() 里的"准备工作"。
 * SDK 帮你自动做了，这里你亲手做。
 *
 * 第①步 GetModel(g_model)：把 Flash 里的字节数组解析为 TFLite 模型
 *    本质是 flatbuffer 反序列化，零拷贝，数据始终在 Flash。
 * 第②步 版本校验：编译时的 TFLite 版本必须和模型的 flatbuffer schema 版本兼容。
 * 第③步 算子注册：嵌入式 AI 最核心的概念。
 *    桌面 TensorFlow 会自动发现模型用了哪些算子。
 *    TFLite Micro 不会——必须手工列出来。
 *    从哪里知道需要哪些算子？用 netron.app 打开 .tflite 文件可视化查看。
 * 第④步 MicroInterpreter 构造 + AllocateTensors：
 *    把所有中间张量（每层的输入输出）在 tensor_arena 里切分好。
 *    从此之后不再需要 malloc——嵌入式友好。
 * 第⑤步 获取输入输出指针：推理时直接往 g_input->data.int8 写，从 g_output 读。
 * ================================================================ */
static bool inference_init(void) {
    // ① 加载模型（零拷贝：g_model 在 Flash 里，GetModel 只返回指针）
    const tflite::Model *model = tflite::GetModel(g_model);

    // ② 版本校验（schema version mismatch → 直接退出，防止后续崩溃）
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema mismatch!");
        return false;
    }

    // ③ 注册算子（逐一列出模型需要的所有操作类型）
    g_resolver.AddConv2D();          // 标准卷积（Conv2D）
    g_resolver.AddDepthwiseConv2D(); // 深度可分离卷积（DepthwiseConv）
    g_resolver.AddMean();            // 均值池化（全局平均池化或降维）
    g_resolver.AddFullyConnected();  // 全连接层（最后分类层）
    g_resolver.AddSoftmax();         // Softmax 输出概率

    // ④ 构建解释器 + 分配张量
    // MicroInterpreter 构造函数接收四样东西：模型、算子列表、内存池、内存池大小
    static tflite::MicroInterpreter s_interpreter(
        model, g_resolver, g_tensor_arena, sizeof(g_tensor_arena));
    g_interpreter = &s_interpreter;

    // AllocateTensors 在 arena 里按顺序为每层输入输出切分空间
    // 如果 arena 不够大会返回 kTfLiteError
    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return false;
    }

    // ⑤ 获取输入输出指针（推理时直接读写，不需要重新获取）
    g_input  = g_interpreter->input(0);   // 第 0 个输入（模型通常只有一个输入）
    g_output = g_interpreter->output(0);  // 第 0 个输出（模型通常只有一个输出）

    ESP_LOGI(TAG, "TFLM ready, arena %d bytes", (int)sizeof(g_tensor_arena));
    return true;
}


/* ================================================================
 * 执行一次推理 — 每次按键拍照后调用
 *
 * 这是 Edge Impulse run_classifier() 的核心三步：
 *   ⑥ 拷贝输入数据 → ⑦ Invoke 推理 → ⑧ 反量化
 *
 * 为什么 memcpy 而不是直接赋值？
 *   g_input->data.int8 指向 tensor arena 里的输入缓冲区。
 *   输入数据在 model_input 数组里（独立分配在 PSRAM）。
 *   memcpy 把 96×96 = 9216 个 int8 拷贝进去。
 *
 * 为什么需要反量化？
 *   模型输出是 INT8 整数，需要转换回 float 概率值：
 *     float_value = (int8_value - zero_point) × scale
 *   你的模型：zero_point = -128, scale = 0.00390625
 *   例：INT8 值 = 30 → (30 - (-128)) × 0.00390625 = 0.617 → Normal
 * ================================================================ */
static bool run_inference(const int8_t *input_data,
                          float *output_probs, int label_count) {
    if (!g_interpreter) return false;

    // ⑥ 把预处理后的数据拷贝到模型输入缓冲区
    // 96×96 = 9216 字节，一次 memcpy 搞定
    memcpy(g_input->data.int8, input_data, 96 * 96);

    // 喂狗（推理 900ms 期间 CPU 不释放，必须喂狗防 WDT 超时）
    esp_task_wdt_reset();

    // ⑦ 执行推理 —— 就这一行！
    // Invoke() 内部循环遍历模型每一层，调用对应算子的 kernel 函数
    inference_pm_acquire();
    const TfLiteStatus invoke_status = g_interpreter->Invoke();
    inference_pm_release();
    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed");
        return false;
    }

    // ⑧ 反量化：INT8 输出 → float 概率
    // 为什么 scale 和 zero_point 在 output->params 里？
    //   这些量化参数在训练时写入模型，由 Edge Impulse / TensorFlow 自动确定。
    //   反量化公式是固定的，不需要自己算 scale。
    float s  = g_output->params.scale;        // 输出量化步长
    int32_t z = g_output->params.zero_point;  // 输出量化零点
    for (int i = 0; i < label_count; i++) {
        output_probs[i] = (g_output->data.int8[i] - z) * s;
    }

    return true;
}


/* ================================================================
 * 板载 RGB 灯初始化 — 上电后把 WS2812 设为蓝色常亮
 *
 * 用 RMT 外设驱动单颗 WS2812（espressif/led_strip 组件）：
 *   led_strip_new_rmt_device()  创建驱动实例（RMT 时钟 10MHz）
 *   led_strip_set_pixel()       设置像素颜色（GRB 顺序，R=0 G=0 B=255）
 *   led_strip_refresh()         把颜色真正发到 LED
 * 受编译期开关 FALL_ENABLE_RGB_LED 控制（device_config.h，默认开）。
 * ================================================================ */
static void rgb_led_init(void) {
#if FALL_ENABLE_RGB_LED
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,                          // 只有 1 颗灯
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,      // 10MHz，WS2812 标准时序
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    led_strip_handle_t strip = nullptr;
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED init failed: 0x%x", err);
        return;
    }
    // 蓝色常亮（R=0, G=0, B=255 → 满亮度蓝）
    err = led_strip_set_pixel(strip, 0, 0, 0, 255);
    if (err == ESP_OK) err = led_strip_refresh(strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB LED set blue failed: 0x%x", err);
        return;
    }
    ESP_LOGI(TAG, "RGB LED (GPIO %d) solid blue", RGB_LED_GPIO);
#endif
}


/* ================================================================
 * app_main — FreeRTOS 任务入口（类似 Arduino 的 setup+loop 合并）
 *
 * 流程：
 *   1. tflite::InitializeTarget()  — TFLite Micro 平台初始化
 *   2. inference_init()            — 加载模型、注册算子、分配内存（一次）
 *   3. button_init()               — 配置 GPIO 42 按键
 *   4. camera_init()               — 初始化 OV2640
 *   5. malloc 帧缓冲 (225KB) 和模型输入缓冲 (9KB)
 *   6. 订阅 WDT
 *   7. while(1): 等按键 → 抓 3 帧 → 逐帧预处理 → 逐帧推理 → 打印
 * ================================================================ */
extern "C" void app_main() {
    // 上电即点亮板载 RGB 灯（蓝色常亮），后续初始化失败也能看到状态
    rgb_led_init();

    ota_init();
    telemetry_init();

    power_management_init();
    fall_network_init();

    // TFLite Micro 平台初始化（记录当前 FreeRTOS 任务，用于错误报告）
    tflite::InitializeTarget();

    // 加载模型 + 注册算子 + 分配 tensor arena（只做一次，后面每次推理复用）
    if (!inference_init()) return;

    printf("\n========================================\n");
    printf("  Pure TFLite Micro — Zero Edge Impulse SDK\n");
    printf("  Press button (GPIO 42) to capture\n");
    printf("========================================\n\n");

    // 按键 + 雷达初始化
    button_init();
    radar_init();
    // 起雷达接收任务（FreeRTOS 任务，一直跑，不阻塞 app_main）
    xTaskCreate(radar_task, "radar", 4096, NULL, 5, NULL);

    // 在 PSRAM 里预分配帧缓冲和模型输入缓冲（启动时分配一次，后续复用）
    uint8_t *rgb_buf = (uint8_t*)malloc(320 * 240 * 3);
    int8_t  *input   = (int8_t*)malloc(96 * 96);
    if (!rgb_buf || !input) {
        ESP_LOGE(TAG, "malloc failed — PSRAM may not be enabled");
        return;
    }

    // 订阅任务看门狗
    esp_task_wdt_add(NULL);

    while (1) {
        printf("Waiting (button or radar fall trigger)...\n");

        // 轮询：等雷达或按键触发（每 100ms 检查一次）
        bool triggered = false;
        float trigger_x = 0.0f;
        float trigger_y = 0.0f;
        float trigger_z = 0.0f;
        while (!triggered) {
            esp_task_wdt_reset();
            if (radar_is_triggered()) {
                float x, y, z, s;
                radar_get_last_target(&x, &y, &z, &s);
                trigger_x = x;
                trigger_y = y;
                trigger_z = z;
                printf("\n*** RADAR TRIGGER: Z=%.2fm, targets=%d ***\n",
                       (double)z, radar_get_target_count());
                radar_clear_trigger();
                triggered = true;
            }
            if (gpio_get_level(BTN_GPIO) == BTN_ACTIVE_LEVEL) {
                vTaskDelay(pdMS_TO_TICKS(80));  // 消抖
                printf("\n*** BUTTON TRIGGER ***\n");
                triggered = true;
            }
            if (!triggered)
                vTaskDelay(pdMS_TO_TICKS(100));
        }

        // ★ 触发后等 2 秒，让人完全倒地后再拍
        printf("Waiting 2s for fall to complete...\n");
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (!camera_init()) {
            ESP_LOGE(TAG, "Camera init failed, skip");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(800));  // 等 AWB/AEC 稳定（加长到 800ms）

        // 拍一张废帧让自动曝光收敛（不推理）
        {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        printf("Capturing %d frames...\n", CAPTURE_COUNT);

        int fall_votes = 0, total_frames = 0;
        for (int n = 0; n < CAPTURE_COUNT; n++) {
            printf("\n--- Frame %d/%d ---\n", n + 1, CAPTURE_COUNT);
            if (!camera_capture(rgb_buf)) { ESP_LOGE(TAG, "Capture failed"); continue; }
            preprocess_rgb_to_int8(rgb_buf, 320, 240, input, 96, 96);

            float probs[2] = {0};
            if (!run_inference(input, probs, 2)) { ESP_LOGE(TAG, "Inference failed"); continue; }
            total_frames++;

            printf("Input[0..7]: ");
            for (int i = 0; i < 8; i++) printf("%d ", input[i]);
            printf("\n  fall: %.5f  normal: %.5f\n", (double)probs[0], (double)probs[1]);

            if (probs[0] > 0.6f) fall_votes++;
        }

        // ★ 融合判决：雷达触发 + 视觉多数票 > 50%
        if (total_frames > 0 && fall_votes > total_frames / 2) {
            printf("\n🚨 ALERT: FALL CONFIRMED! (radar + vision: %d/%d frames agree)\n",
                   fall_votes, total_frames);
            radar_mark_confirmed();  // 进入冷却期
            // 阶段2：异步入队立即返回（不阻塞推理）；服务器离线时由
            // network_task 指数退避补传。旧链路（编译开关=0）为同步发送。
            const bool uploaded = fall_network_submit_fall_event(
                trigger_x, trigger_y, trigger_z, fall_votes, total_frames);
            if (!uploaded) {
                ESP_LOGW(TAG, "fall result was not uploaded");
            }
            // V2 迭代标记：跌倒事件上报后打印，供遥测/日志验证迭代链路
            ESP_LOGI(TAG, "FALL DETECTED V2");
        } else {
            printf("\n⚠️  Radar trigger NOT confirmed by vision (%d/%d frames agree)\n",
                   fall_votes, total_frames);
        }

        camera_deinit();
        printf("\nDone! Ready for next trigger.\n\n");
    }
}
