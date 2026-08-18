/* 跌倒板配置（验证：同模板可复现）*/
#define BOARD_DEVICE_ID      "esp32s3-cam-01"
#define BOARD_MODEL          "esp32s3-cam-fall"
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
#define BOARD_FW_VERSION      "0.1.0"
