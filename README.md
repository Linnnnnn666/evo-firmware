# EvoAgent — 自进化的 AI 硬件开发系统

> **一句话**：用语音或文字指挥 AI 智能体，为 ESP32 板卡完成「写固件 → 编译 → OTA 部署 → 遥测验收 → 经验沉淀」的完整开发闭环——**核心链路已验证、架构完整、持续迭代中的系统原型**，双层自进化，人在环兜底。

> **English abstract**: EvoAgent firmware collection for ESP32-S3 boards — an **AI-driven hardware development loop** in which an LLM agent writes firmware, builds it, deploys it via **dual-partition OTA**, and validates it through **telemetry self-checks** with a human-in-the-loop gate. Boards: fall-detection (on-device AI + radar), cloud-programming board, and a configurable template firmware. Built on **ESP-IDF / esp32 / embedded C++**, MIT licensed. Part of the self-evolving EvoAgent system — see [evo-fall-mcp](https://github.com/Linnnnnn666/evo-fall-mcp) (capability hub) and [evo-voice-terminal](https://github.com/Linnnnnn666/evo-voice-terminal) (voice entry).

```
                              ┌──────────────┐
                              │     用户     │
                              └──────┬───────┘
                语音「你好小安」        │       文字（DSH 会话）
                     │               │              │
                     ▼               ▼              ▼
        ┌──────────────────┐  ┌────────────────────────────┐
        │ 语音链路         │  │ 智能体层                    │
        │                  │  │ ┌──────────────────────┐   │
        │ 语音板           │  │ │ DSH-1 干活者         │    │
        │ (evo-voice-      │  │ │ 写代码/编译/部署/排障 ◄── ┼── 插件装入
        │  terminal)       │  │ └──────────┬───────────┘    │
        │   │ WS/opus      │  │            │ 能力缺口       │
        │   ▼              │  │            ▼                │
        │ xiaozhi-server   │  │ ┌──────────────────────┐    │
        │ ASR→LLM→TTS      │  │ │ DSH-2 进化者          │   │
        └────────┬─────────┘  │ │ (隔离环境开发插件)     │──┼── req_*.json
                 │            │ └──────────────────────┘    │
                 │ 工具调用    └────────────────────────────┘
                 ▼
        ┌───────────────────────────────────────────────────┐
        │ 能力中枢 (fall-mcp) —— 47 工具                    │
        │ 部署/烧录/播报/自验收/门控/工具工厂/经验库/插件轮询│
        └───────┬──────────────────────────┬────────────────┘
                │ MQTT / HTTP / OTA        │ 遥测 · 事件回流
                ▼                          ▲
        ┌───────────────────────────────────────────────┐
        │ 硬件层 —— ESP32 板卡                          │
        │ 跌倒检测板 · 云端烧录板 · 业务板（OTA 双分区） │
        └───────────────────────────────────────────────┘

   进化回流：工具工厂/经验库 → 注入下一次任务 · DSH-2 插件 → 装入 DSH-1
   人在环：关键决策经语音板播报确认（confirm 队列）——AI 全自动不可信
```

## 系统的灵魂：双层自进化

**这不是一个"用 AI 写固件"的项目，而是一个"AI 自己给自己升级能力"的系统。**
系统不仅越用越熟练（经验沉淀），还能**自己发现自己缺什么能力、自己把能力造出来装上**——两层进化闭环。

### 第一层 · 系统自进化 —— 进化"手"（工具与经验）

每干完一次活自动复盘：需求值得固化？→ 工具工厂生成 MCP 工具（编译+验证才注册）；
任务结果沉淀进经验库（bigram 索引），下次相似任务自动注入参考经验。
真实案例："接入一块新板" → 从零建出 **board-template 通用引导固件** →
之后每块新板复用模板（**固件能力模板化**——本仓库就是这层进化的成果）。

### 第二层 · 智能体自进化 —— 进化"大脑"（DSH 插件）

DSH 双角色：**DSH-1 干活，DSH-2 进化**。DSH-1 干完活复盘自己的能力缺口 →
写插件需求文件 → DSH-2 在**隔离环境**开发插件 → 装入 DSH-1 → 健康检查；
装坏了 DSH-2 修复，坏插件移入 `quarantine/` 隔离区。
真实案例：DSH-2 造出 `base64-codec` / `reverse-string` / `text-stats` 插件给 DSH-1 装上。

### 闭环与保险

```
DSH-1 干活 → 复盘① 固化工具/模板（手变强）/ 复盘② 发现缺口 → req_*.json
   → DSH-2 隔离造插件 → 装入 DSH-1（大脑变强）→ 干得更好 → 经验更多 → 螺旋上升
保险：DSH-2 隔离开发 · 工具编译验证 · 插件健康检查 · quarantine 可回滚 · 人在环兜底
```

**进化的是能力容器（工具/插件/经验），不碰模型**——可控、可解释、可回滚。
本仓库的每块板卡都为此设计：遥测、OTA、可观测性——让"AI 写固件 → 系统验收 → 能力沉淀"的闭环成为可能。

## 两个支撑信念

1. **人在环验收**：系统先自己验（遥测断言），验不了/验不过才语音问你——AI 全自动不可信，人在环是信任底座。
2. **AI 协作开发**：AI 写代码、编译、部署；人类负责架构、硬件驱动、排障、最终验收。

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

> 分层入口：**[0 层 · 先看效果](https://github.com/Linnnnnn666/evo-fall-mcp)**（系统架构图）· **[1 层 · 纯软件 5 分钟](https://github.com/Linnnnnn666/evo-fall-mcp#快速开始)**（能力中枢，无需硬件）·
> **[2 层 · 单板体验](#快速开始)**（本仓库：一块 ESP32-S3 跑起来）· **[3 层 · 完整系统](https://github.com/Linnnnnn666/evo-fall-mcp/blob/main/docs/QUICK_START.md)**（服务器+中枢+固件+语音板全链路）

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
