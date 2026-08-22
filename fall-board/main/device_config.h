/*
 * 设备配置（公共头）
 *
 * 优先加载私有配置 device_config_private.h（真实值，不入库，见 .gitignore）；
 * 若不存在则使用下面的占位默认值。首次使用：
 *   1. cp main/device_config.h.example main/device_config_private.h
 *   2. 填入真实 Wi-Fi / 服务器 / token
 *
 * 任务书 6.2：秘密不得提交 Git，仓库只保留 .example。
 */
#pragma once

#if __has_include("device_config_private.h")
#include "device_config_private.h"
#else
#define DEVICE_WIFI_SSID       "YOUR_WIFI_SSID"      /* 填入真实 SSID */
#define DEVICE_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"  /* 填入真实密码 */
#define DEVICE_API_BASE_URL    "http://192.168.1.100:8000"  /* 自有服务器（无域名阶段用公网 IP） */
#define DEVICE_TOKEN           "REPLACE_WITH_DEVICE_TOKEN"
#define DEVICE_ID              "esp32s3-cam-01"
#endif

/* 固件/模型版本（随上报与心跳发送，服务器用于 OTA 判断） */
#ifndef FALL_FW_VERSION
#define FALL_FW_VERSION        "1.8.0"
#endif
#ifndef FALL_MODEL_VERSION
#define FALL_MODEL_VERSION     "fall-int8-v1"
#endif

/*
 * 编译期开关（任务书 13.9：新功能必须能通过编译期开关关闭）：
 *   FALL_ENABLE_DIRECT_SERVER = 1  新链路：异步队列直连自有服务器（默认）
 *                              0  旧链路：同步发送到本地电脑接口（回退）
 *   FALL_ENABLE_HEARTBEAT     = 1  每 5 分钟发送心跳（默认）
 *   FALL_ENABLE_RGB_LED       = 1  上电后板载 WS2812 RGB 灯常亮蓝色（默认）
 */
#ifndef FALL_ENABLE_DIRECT_SERVER
#define FALL_ENABLE_DIRECT_SERVER 1
#endif
#ifndef FALL_ENABLE_HEARTBEAT
#define FALL_ENABLE_HEARTBEAT 1
#endif
#ifndef FALL_ENABLE_RGB_LED
#define FALL_ENABLE_RGB_LED 1
#endif

/* ── OTA + 遥测（引导固件能力层）── */
#define FW_VERSION             FALL_FW_VERSION
#define OTA_LATEST_URL         DEVICE_API_BASE_URL "/firmware/fall-board/latest.json"
#define MQTT_HOST              "YOUR_SERVER_HOST"
#define MQTT_PORT              1883
#define MQTT_USER              "YOUR_MQTT_USER"
#define MQTT_PASS              "YOUR_MQTT_PASSWORD"
#define TELEMETRY_TOPIC        "fall/telemetry/" DEVICE_ID
#define TELEMETRY_INTERVAL_S   10
