/* 新板（2026-08-21 到货，MAC xx:xx:xx:xx:xx:xx）：普通 ESP32-S3 开发板 */
#define BOARD_DEVICE_ID      "board-s3-5798"
#define BOARD_MODEL          "esp32s3-generic"
#define BOARD_WIFI_SSID      "YOUR_WIFI_SSID"
#define BOARD_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define BOARD_API_BASE      "http://YOUR_SERVER_HOST"
#define BOARD_MQTT_HOST      "YOUR_SERVER_HOST"
#define BOARD_MQTT_PORT      1883
#define BOARD_MQTT_USER      "YOUR_MQTT_USER"
#define BOARD_MQTT_PASS      "YOUR_MQTT_PASSWORD"
#define BOARD_TELEMETRY_TOPIC "fall/telemetry/" BOARD_DEVICE_ID
#define BOARD_COMMAND_TOPIC   "fall/commands/" BOARD_DEVICE_ID
#define BOARD_LED_GPIO       48
#define BOARD_LED_WS2812     1   /* 板载 RGB 灯为 WS2812 可寻址灯珠（GPIO 48） */
#define BOARD_LED_CYCLE_RGB  1   /* 呼吸灯：红→蓝→绿→红循环渐变（~3.6s 一循环，0→亮→0） */
#define BOARD_FW_VERSION      "0.9.0"
