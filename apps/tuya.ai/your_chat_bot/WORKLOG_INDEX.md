# your_chat_bot 项目持续上下文入口

这个文件用于恢复 `apps/tuya.ai/your_chat_bot` 项目的长期上下文。以后重新打开项目、开启新对话或更换 Codex/Cursor 线程时，优先阅读本文档，再按需要打开对应日期的详细记录。

## 快速恢复顺序

1. 先读本文档，了解当前项目的关键背景和记录索引。
2. 如果任务涉及机器狗控制、手柄、语音或 MCP，优先读：
   - `2026-06-30_robot_control_fixes_summary.md`
3. 如果任务涉及 BLE 手柄连接、配网共存或启动重连，按时间顺序读：
   - `2026-06-02_quaddle_gamepad_integration_summary.md`
   - `2026-06-03_quaddle_ble_hid_debug_worklog.md`
   - `2026-06-04_quaddle_ble_hid_notify_poll_fix_worklog.md`
   - `2026-06-05_quaddle_ble_tuya_provisioning_worklog.md`
   - `2026-06-08_quaddle_ble_boot_netcfg_reconnect_worklog.md`
4. 如果任务涉及 USB Type-C 日志、CLI 输入或模拟手柄事件，读：
   - `2026-06-02_usb_jtag_cli_worklog.md`

## 当前核心背景

- 当前子项目路径：`apps/tuya.ai/your_chat_bot`
- 目标硬件：TuyaOpen / ESP32-S3 AI_BOARD
- 机器狗控制串口：UART1，GPIO17/18，115200 8N1
- BLE 手柄参考项目：`/home/jasonw/Projects/joyDemoS11/QuaddleGame`
- 主要控制入口：
  - BLE HID 手柄
  - 本地 ASR 语音规则
  - 云端 MCP `self.robot.send_command`
  - CLI/测试串口命令
- 机器人动作发送和仲裁主要集中在：
  - `src/quaddle_robot_bridge.c`
  - `src/quaddle_ble_hid_central.c`
  - `src/robot_uart_voice.c`
  - `src/ai_mcp_robot_tools.c`
  - `src/second_uart.c`

## 最近重要状态

### 2026-06-30 机器人控制链路修复

详见 `2026-06-30_robot_control_fixes_summary.md`。

已完成内容：

- 同步 Arduino 参考项目中的最新蓝牙手柄遥控逻辑。
- 修复重复按同一个手柄按钮不会重复执行动作的问题。
- 将语音、MCP、CLI、默认测试命令中的后退动作从旧格式 `kbk` 迁移到 `kbkF`。
- 在 MCP 命令入口保留旧格式兼容，将 `kbk ...` 自动归一化为 `kbkF ...`。
- 在 `quaddle_robot_bridge.c` 增加 ASR/MCP 5 秒去重，避免同一语音意图被本地 ASR 和云端 MCP 重复执行。
- 构建验证曾通过：

```bash
. ./export.sh && cd apps/tuya.ai/your_chat_bot && tos.py build
git diff --check
```

当时 `tools/check_format.py` 因环境缺少 `clang-format` 未完成，不是代码编译错误。

### 2026-06-02 到 2026-06-08 BLE 手柄与配网链路

这些记录覆盖了从 Arduino 手柄项目移植到 TuyaOpen、USB CLI 调试、BLE HID 报文解析、Notify/Poll 接收、Tuya BLE 配网共存、BOOT 键触发配网、配网后恢复手柄连接等工作。

## 记录索引

| 日期 | 文件 | 主题 |
| --- | --- | --- |
| 2026-06-02 | `2026-06-02_quaddle_gamepad_integration_summary.md` | 将 Quaddle Arduino 手柄控制逻辑整合进 TuyaOpen |
| 2026-06-02 | `2026-06-02_usb_jtag_cli_worklog.md` | AI_BOARD USB Type-C CLI、日志与模拟手柄事件调试 |
| 2026-06-03 | `2026-06-03_quaddle_ble_hid_debug_worklog.md` | BLE HID 手柄连接、报文解析与调试 |
| 2026-06-04 | `2026-06-04_quaddle_ble_hid_notify_poll_fix_worklog.md` | BLE Notify 接收与摇杆回中误发修复 |
| 2026-06-05 | `2026-06-05_quaddle_ble_tuya_provisioning_worklog.md` | BLE 手柄与 Tuya App 配网共存 |
| 2026-06-08 | `2026-06-08_quaddle_ble_boot_netcfg_reconnect_worklog.md` | 手柄优先连接、BOOT 触发配网、配网后恢复手柄 |
| 2026-06-30 | `2026-06-30_robot_control_fixes_summary.md` | 手柄映射同步、重复按键修复、`kbkF`、ASR/MCP 去重 |
| 2026-07-03 | `2026-07-03_quaddle_gamepad_switch_summary.md` | 新增 Quaddle 手柄连接与操控总开关 |

## 给后续 Codex/Cursor 的约定

- 在修改 `your_chat_bot` 前，先检查本索引是否提到相关历史记录。
- 对机器人动作命令、手柄映射、BLE 连接、语音规则、MCP 工具描述的改动，要同步更新对应日期记录或新增日期记录。
- 如果一次任务解决了新的硬件现象、日志现象或控制链路问题，请新增 `YYYY-MM-DD_<topic>_worklog.md` 或 `YYYY-MM-DD_<topic>_summary.md`，并把它加入本索引。
- 不要把原始聊天线程当作唯一上下文来源；重要结论应沉淀到本目录的 Markdown 记录中。
