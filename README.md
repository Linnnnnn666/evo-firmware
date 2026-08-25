# EvoAgent — 自进化的 AI 硬件开发系统

> **一句话**：用语音或文字指挥 AI 智能体，为 ESP32 板卡完成「写固件 → 编译 → OTA 部署 → 遥测验收 → 经验沉淀」的完整开发闭环，系统自主迭代，人在环兜底。

```
  用户语音「你好小安」              用户文字（DSH 会话）
        │                                │
        ▼                                ▼
 ┌───────────────┐              ┌──────────────────┐
 │  语音板        │   WS/opus    │  DSH              │
 │ (evo-voice-   │ ───────────► │ (DeepSeek Harness)│
 │  terminal)    │              │  AI 智能体         │
 └───────▲───────┘              └────────┬─────────┘
         │ TTS 播报                      │ MCP 协议
         │                               ▼
         │                      ┌──────────────────┐   MQTT / HTTP    ┌──────────────┐
         └──────────────────────│  能力中枢         │ ───────────────► │ ESP32 板卡    │
                                │  (fall-mcp)      │ ◄─────────────── │ 跌倒检测/烧录 │
                                │  47 个工具        │   遥测/事件回流   │ /业务板      │
                                └────────┬─────────┘                  └──────────────┘
                                         │
                                         ▼
                            工具工厂 · 经验库 · 人在环验收
                            （同样的坑，系统不犯第二次）
```

## 系统的灵魂：三个设计信念

1. **自进化（能力容器，而非模型）**
   系统每解决一个问题，就把经验沉淀进可检索的知识库（RAG + 置信度）；AI 可以自造新工具（工具工厂）——模型不变，但系统越用越熟练。进化的能力容器（工具/经验/资产），不碰模型，可控且可解释。

2. **人在环验收（AI 可靠性的底座）**
   系统先自己验：遥测字段断言（如 `led_color=#0000FF`），验不了/验不过才语音问你。修复循环最多 3 轮，再不行求助人类。AI 全自动不可信，人在环是信任底座。

3. **AI 协作开发（工程师的进化方向）**
   AI 写代码、编译、归档、部署；人类负责架构、硬件驱动、全链路排障、最终验收。这个仓库群就是这套协作模式的完整实践。

## 仓库地图（三件套）

| 仓库 | 角色 | 一句话 |
|------|------|--------|
| **[evo-firmware](https://github.com/Linnnnnn666/evo-firmware)** | 硬件端 | ESP32-S3 固件集合：跌倒检测板（端侧 AI）、云端烧录板、配置化引导固件 |
| **[evo-fall-mcp](https://github.com/Linnnnnn666/evo-fall-mcp)** | 能力中枢 | MCP 服务器（47 工具）：部署/烧录/播报/自验收/自进化，连接 AI 与硬件 |
| **[evo-voice-terminal](https://github.com/Linnnnnn666/evo-voice-terminal)** | 语音入口 | 语音板板卡包：唤醒「你好小安」→ 语音对话 → TTS 播报 |

**本仓库是其中的「硬件端」**——所有被 AI 指挥、被系统验收的 ESP32-S3 板卡固件都在这里。每块板都有遥测、OTA、可观测性设计，让"AI 写固件 → 系统验收"的闭环成为可能。

---

# EvoAgent Firmware

EvoAgent 硬件端固件集合 —— 一套由语音驱动的自进化 AI 硬件开发系统的 ESP32-S3 固件。

> 语音板固件（xiaozhi-esp32 定制）见姊妹仓库 **[evo-voice-terminal](https://github.com/Linnnnnn666/evo-voice-terminal)**。

## 固件工程

| 工程 | 板卡角色 | 说明 |
|---|---|---|
| `board-template/` | 通用业务板 | 配置化引导固件：`main/boards/<板名>.h` 定义 WiFi/MQTT/OTA/LED，双分区 OTA，10s 遥测，呼吸灯/常亮灯效 |
| `esp-flasher-proto/` | 云端烧录板 | MQTT 指令执行器：订阅 `fall/commands/flasher-board`，接收 `flash_start/abort/status`，通过串口给目标板烧录固件，日志回传 |
| `fall-board/` | 跌倒检测板 | **端侧 AI**：LD6002B 60GHz 毫米波雷达实时跌倒判定（TinyFrame 协议解析 + Z 轴状态机 + 抗误报），WiFi 直连上报 + OTA 升级 |
| `oled-display/` | OLED 显示板（已改造为语音板） | 订阅烧录事件流，SSD1306 显示烧录进度 HUD；**注：该板 2026-08-25 已改造为语音板（见 evo-voice-terminal 仓库），本固件保留作备份/回刷用途** |

## 一次真实的系统迭代（以跌倒板为例）

```
DSH 改完固件代码 → dev_ota_deploy 编译归档 app.bin
   → 发布 ota_check（MQTT 命令）
   → 板卡下载 OTA 升级并重启（双分区，失败自动回滚）
   → 10s 遥测回流（fw 版本 / uptime / ota_err…）
   → 系统断言版本号变化 → 「已验证」
   → 播报"新固件已装好"（语音板 TTS）
```

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

## 端侧 AI：跌倒检测（fall-board）

- **雷达**：LD6002B 60GHz 毫米波，UART 115200，TinyFrame 帧协议（SOF/ID/LEN/TYPE/双校验）
- **数据**：3D 点云（target_num + x/y/z/speed，float 米/米每秒）
- **判定**：实时状态机——站立基线确认（z≥1.2m×3 帧）→ 1s 窗口内倒地（z≤0.8m×2 帧）+ Z 轴下降速度 ≥1m/s → 触发报警
- **抗误报**：单帧不触发（防解析噪声）、连续帧 streak 确认、合理性过滤（z∈0.05~8m）、3s 冷却
- **演进**：摄像头 + TFLite Micro 多数票复核（雷达初筛 + 视觉二次确认）

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

## 迭代历史

- `feat(board-template)` 配置化引导固件：双分区 OTA + 10s 遥测 + LED 灯效（08-18）
- `feat(flasher)` 云端烧录板：MQTT 指令执行器 + 事件/日志回传（08-20）
- `feat(oled-display)` SSD1306 HUD 显示板固件（08-21）
- `feat(fall-board)` 跌倒检测板：LD6002B 雷达端侧实时判定（08-22）
- `docs` README + MIT 许可（08-23）

## License

MIT © 2026 EvoAgent

> ⚠️ 仓库内所有真实凭据已替换为 `YOUR_*` 占位符。请勿提交真实 WiFi 密码 / MQTT 凭据 / 服务器地址。
