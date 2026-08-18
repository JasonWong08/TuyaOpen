# 2026-08-18 AI UART 重复命令误判修复

## 现象

语音指令“向前走两步”能够被 ASR 正确解析为 `kwkF 2`，也有语音回复，但机器人没有动作。日志中出现了仲裁层的 `AI command sent`，却没有对应的 `second_uart: TX` 和机器人完成令牌。相比之下，`khi` 打招呼命令同时具备 TX 日志和完成令牌，能够正常执行。

## 根因

`second_uart_send_string()` 会抑制与上一次 UART 数据完全相同的命令。命中重复数据时不会调用 `tal_uart_write()`，但仍返回 `OPRT_OK`。AI 仲裁层仅根据返回值判断发送成功，因此会错误打印 `AI command sent` 并等待一个不会到达的完成令牌。

ASR 与 MCP 的短时间重复调用已经在 `quaddle_robot_bridge_queue_ai_command()` 中单独处理，因此离散 AI 命令无需再经过 UART 层的永久上一包重复抑制。

## 修复

在 `quaddle_robot_bridge_poll()` 的 AI 仲裁发送路径中，将：

```c
second_uart_send_string(s_pending_ai_cmd)
```

改为：

```c
second_uart_send_string_force(s_pending_ai_cmd)
```

这样相同的离散语音动作可以再次实际写入 UART，同时保留现有的 ASR/MCP 5 秒防重逻辑。

## 验证

执行：

```bash
. ./export.sh && cd apps/tuya.ai/your_chat_bot && tos.py build
git diff --check
```

ESP32-S3 `AI_BOARD` 构建成功，固件输出到：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.0
```

`tools/check_format.py` 因当前环境没有 `clang-format` 无法运行；本次源代码仅替换发送函数并增加一行说明注释，未改变原有排版结构。

上板后重复发出“向前走两步”，预期每次都能看到：

```text
second_uart: TX UART1 GPIO17/18 7 bytes: "kwkF 2\n"
robot arbitration: robot token 'k' completed "kwkF 2"
```
