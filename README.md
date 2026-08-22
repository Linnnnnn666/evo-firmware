# EvoAgent Firmware

EvoAgent 硬件端固件集合 —— 一套由语音驱动的自进化 AI 硬件开发系统的 ESP32-S3 固件。

> 语音板固件（xiaozhi-esp32 定制）见姊妹仓库 **[evo-voice-terminal](https://github.com/Linnnnnn666/evo-voice-terminal)**。

## 架构

```
语音（小安） → LLM/DSH（云端） → MQTT/OTA（服务器） → 本仓库固件（ESP32-S3 板卡）
```

四个固件工程：

| 工程 | 板卡角色 | 说明 |
|---|---|---|
| `board-template/` | 通用业务板 | 配置化引导固件：`main/boards/<板名>.h` 定义 WiFi/MQTT/OTA/LED，双分区 OTA，10s 遥测，呼吸灯/常亮灯效 |
| `esp-flasher-proto/` | 云端烧录板 | MQTT 指令执行器：订阅 `fall/commands/flasher-board`，接收 `flash_start/abort/status`，通过串口给目标板烧录固件，日志回传 |
| `oled-display/` | OLED 显示板（已改造为语音板） | 订阅烧录事件流，SSD1306 显示烧录进度 HUD；**注：该板 2026-08-25 已改造为语音板（见 evo-voice-terminal 仓库），本固件保留作备份/回刷用途** |
| `fall-board/` | 跌倒检测板 | 毫米波雷达跌倒检测，WiFi 直连上报 + OTA 升级 |

## 快速开始

### 环境

- ESP-IDF v5.5.4（`export IDF_TOOLS_PATH=/opt/esp && . /opt/esp-idf/export.sh`）
- ESP32-S3 开发板（N16R8 等）

### 1. 配置板卡

复制模板创建你的板卡配置，填写真实凭据：

```bash
cd board-template
cp main/boards/board.example.h main/boards/my-board.h
# 编辑 my-board.h：BOARD_DEVICE_ID / BOARD_WIFI_SSID / BOARD_MQTT_PASS ...
```

`board.example.h` 中的 `YOUR_*` 占位符替换为实际值（WiFi 账号密码、MQTT 用户名密码、服务器地址）。

### 2. 编译

```bash
cd board-template
idf.py set-target esp32s3
idf.py build          # 或 bash build_board.sh my-board（多板并行编译）
```

### 3. 烧录（首次 USB）

```bash
esptool.py --chip esp32s3 -p COMxx -b 460800 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/board_template.bin
```

### 4. OTA 升级（后续）

服务器归档 `/ota/<device>/v<版本>/app.bin`（**纯 app 镜像**，merged.bin 仅供串口/云端烧录——
直接 OTA 会镜像校验失败），`latest.json` 的 `bin` 字段指向 `app.bin`；
向板子 MQTT 主题 `fall/commands/<device>` 发布纯文本命令 `ota_check`，板子自动下载升级并重启。
板子另有兜底：开机 30 秒自动检查 + 每 6 小时定时检查。

## 设备通信协议

- **遥测**：`fall/telemetry/<device>` JSON `{fw, uptime_s, free_heap, rssi, restart_reason, led, led_color, pwm_duty, ota_err}`（10s）
  - `ota_err`：最后一次 OTA 失败原因（如 `ota_fail_3:ESP_ERR_OTA_VALIDATE_FAILED`），服务器可远程诊断
- **命令**：`fall/commands/<device>` 纯文本 `ota_check` / `led_on` / `led_off` / `led_green_on` / `led_blue_on`
- **烧录板**：`fall/commands/flasher-board` JSON `{"cmd":"flash_start","device":"<目标>"}` / `{"cmd":"abort"}` / `{"cmd":"status"}`
- **烧录事件**：`fall/flasher/events` `ready/waiting/connected/progress/done/error`
- **烧录日志**：`fall/flasher/log`（目标板 UART 回显，`TGT>` 前缀）

## OTA 可靠性设计

- 下载失败自动重试 2 次（间隔 3s/5s），单次超时 60s
- 失败原因上报 `ota_err` 遥测字段（服务器诊断不再依赖串口）
- 双分区 OTA + 失败自动回滚到上一版本

## 目录结构（board-template）

```
main/
├── main.cpp          # 入口：WiFi/MQTT/OTA/遥测/LED 主循环
├── boards/           # 板卡配置（每板一个 .h，含 board.example.h 模板）
├── board_config.h    # 当前活动板卡选择
├── network.cpp       # WiFi/MQTT 连接管理（自动重连）
├── ota.cpp           # 双分区 OTA 升级
└── telemetry.cpp     # 遥测上报
```

## License

MIT © 2026 EvoAgent

> ⚠️ 仓库内所有真实凭据已替换为 `YOUR_*` 占位符。请勿提交真实 WiFi 密码 / MQTT 凭据 / 服务器地址。
