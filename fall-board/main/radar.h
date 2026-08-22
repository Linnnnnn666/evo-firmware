/* ================================================================
 * LD6002B 60GHz 毫米波雷达 — 协议解析 + 跌倒状态机
 *
 * 协议：UART 115200-8N1，TF 帧格式
 * SOF(0x01)|ID(2B)|LEN(2B)|TYPE(2B)|HEAD_CKSUM(1B)|DATA(N)|DATA_CKSUM(1B)
 *
 * 消息类型 0x0A08 = 3D 点云数据
 *   DATA: target_num(4B) | [cluster(4B)|x(4B)|y(4B)|z(4B)|speed(4B)]*N
 *   x/y/z: float，单位 米；speed: float，单位 m/s
 *
 * 引脚：UART1 — TX=GPIO1, RX=GPIO2 (果云ESP32-S3-Cam 空闲IO)
 * ================================================================ */

#ifndef RADAR_H_
#define RADAR_H_

#include <stdint.h>
#include <stdbool.h>

// ---- UART 配置 ----
#define RADAR_UART_PORT    UART_NUM_1
#define RADAR_TX_PIN       GPIO_NUM_1
#define RADAR_RX_PIN       GPIO_NUM_2
#define RADAR_BAUDRATE     115200
#define RADAR_BUF_SIZE     1024          // UART 接收缓冲
#define TF_MAX_PAYLOAD      512           // TF 帧最大负载

// Temporary mode: the powered test board owns radar configuration and ESP32
// only listens to its UART TX on GPIO2. Set to 0 after installing a dedicated
// 3.3V / >=1A radar supply and wiring GPIO1 to radar RX.
#define RADAR_TESTBOARD_RX_ONLY  1

// ---- 协议常量 ----
#define TF_SOF             0x01          // 起始帧头
#define TF_TYPE_POINTCLOUD 0x0A08        // 3D 点云上报

// ---- 跌倒判断阈值 ----
// 演示视频先使用宽松模式：雷达只负责快速初筛，后面仍由摄像头
// TFLite Micro 多数票复核。正式测试时把它改为 0，恢复严格判定。
#define RADAR_DEMO_MODE    1

#if RADAR_DEMO_MODE
#define FALL_Z_HIGH        1.20f         // 高于此值认为是站立 (米)
#define FALL_Z_LOW         1.05f         // 演示模式：低于此值即可进入低位
#define FALL_MAX_DT_SEC    3.0f          // 演示模式允许动作在 3 秒内完成
#define FALL_HIGH_STREAK_REQUIRED 1      // 演示模式：1 帧建立站立基线
#define FALL_LOW_STREAK_REQUIRED  1      // 演示模式：1 帧触发疑似跌倒
#define FALL_COOLDOWN_MS   2500          // 演示时缩短冷却
#else
#define FALL_Z_HIGH        1.2f          // 高于此值认为是站立 (米)
#define FALL_Z_LOW         0.8f          // 低于此值认为跌倒/蹲下 (米)
#define FALL_MAX_DT_SEC    1.0f          // 从站立→倒地必须在 1 秒内发生
#define FALL_HIGH_STREAK_REQUIRED 3
#define FALL_LOW_STREAK_REQUIRED  2
#define FALL_COOLDOWN_MS   3000          // 报警后 3 秒冷却
#endif

#define FALL_DROP_SPEED    1.0f          // Z 下降速度阈值 (m/s)

// ---- 雷达初始化/反初始化 ----
void radar_init(void);
void radar_task(void *arg);              // FreeRTOS 任务入口
bool radar_is_triggered(void);           // 雷达检测到疑似跌倒？
void radar_clear_trigger(void);          // 清除触发标志（视觉处理后调用）
void radar_mark_confirmed(void);         // 视觉确认后标记，进入冷却
int  radar_get_target_count(void);       // 当前检测到的目标数

// ---- 获取最近一次的目标数据 ----
void radar_get_last_target(float *x, float *y, float *z, float *speed);

#endif
