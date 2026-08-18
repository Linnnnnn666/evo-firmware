/* ESP32-S3 怪板（已修复）（xx:xx:xx:xx:xx:xx）：board-s3-477c */
#define BOARD_DEVICE_ID      "board-s3-477c"
#define BOARD_MODEL          "ESP32-S3 怪板（已修复）"
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
#define BOARD_LED_WS2812     1              /* 板载 RGB 灯为 WS2812 可寻址灯珠（GPIO 48） */
#define BOARD_LED_COLOR_GRB  0, 0, 255      /* 上电后蓝色常亮（G,R,B） */
#define BOARD_FW_VERSION      "0.2.0"
