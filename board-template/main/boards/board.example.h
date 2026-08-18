/* EvoAgent 板卡配置模板
 *
 * 使用方法：
 *   1. cp boards/board.example.h boards/<你的板名>.h
 *   2. 填写真实凭据（YOUR_* 占位符）
 *   3. 修改 main/board_config.h 中的 BOARD_DEVICE_ID 指向新板名
 */
#ifndef BOARD_EXAMPLE_H
#define BOARD_EXAMPLE_H

/* 板卡身份 */
#define BOARD_DEVICE_ID      "board-example"            /* 唯一设备 ID（用于 MQTT 主题） */
#define BOARD_MODEL          "esp32s3-generic"

/* 网络凭据（必填：替换 YOUR_* 为实际值） */
#define BOARD_WIFI_SSID      "YOUR_WIFI_SSID"
#define BOARD_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

/* 服务器 */
#define BOARD_API_BASE       "http://YOUR_SERVER_HOST"
#define BOARD_MQTT_HOST      "YOUR_SERVER_HOST"
#define BOARD_MQTT_PORT      1883
#define BOARD_MQTT_USER      "YOUR_MQTT_USER"
#define BOARD_MQTT_PASS      "YOUR_MQTT_PASSWORD"

/* MQTT 主题（一般无需修改） */
#define BOARD_TELEMETRY_TOPIC "fall/telemetry/" BOARD_DEVICE_ID
#define BOARD_COMMAND_TOPIC   "fall/commands/" BOARD_DEVICE_ID

/* 板载 LED */
#define BOARD_LED_GPIO       48
#define BOARD_LED_WS2812     1      /* 1=WS2812 可寻址灯珠，0=普通 GPIO LED */
#define BOARD_LED_CYCLE_RGB  1      /* 呼吸灯：红→蓝→绿循环渐变（~3.6s 一循环，亮→灭） */

/* 固件版本（OTA 版本递增依据） */
#define BOARD_FW_VERSION     "0.1.0"

#endif /* BOARD_EXAMPLE_H */
