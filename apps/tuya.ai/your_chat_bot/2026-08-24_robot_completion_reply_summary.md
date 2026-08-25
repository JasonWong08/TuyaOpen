# 2026-08-24 机器人完成令牌与语音回复联动修复

## 现象

用户要求机器人执行 `kwkF 5` 时，云端在机器人实际完成前回复“已经向前走了五步”。原有 MCP 工具只等待命令进入 Tuya 侧队列，返回 `ok` 并不代表机器人已经执行完成。本地 ASR 已经排队相同动作时，后续 MCP 调用会被去重，但同样返回 `ok`，进一步放大了误报完成的问题。

## 修复

### 命令跟踪

`quaddle_robot_bridge.c` 为每条 AI 命令分配非零 ticket，并让队列、当前命令和最近 ASR 去重记录携带该 ticket。

- 新 MCP 命令等待自己对应的 ticket。
- MCP 命中最近 ASR 去重时，复用该 ASR 命令的 ticket，不重复发送动作，但仍等待真实动作完成。
- 收到机器人返回的完整单字符完成行（例如 `k\n`）后，才将对应 ticket 标记为完成。
- UART 发送失败、完成令牌超时、手柄抢占或队列清理会将对应 ticket 标记为失败。

### MCP 返回语义

`self.robot.send_command` 现在最多等待 21 秒：

- 收到对应完成令牌后返回 `completed: robot completion token received`。
- 失败或超时返回 `robot action not completed; do not claim completion`。

工具描述和补充提示词同步要求：等待期间只能表达“正在执行”；只有工具确认收到完成令牌后才能表达“已经完成”；失败或超时只能说明无法确认完成。

## 预期日志

完整执行时：

```text
robot arbitration: AI command queued "kwkF 5" from ASR depth=1 ticket=1
robot arbitration: MCP command "kwkF 5" skipped; recent ASR command "kwkF 5" already queued
MCP robot send_command tracking segment 1 "kwkF 5" ticket=1
robot arbitration: robot token 'k' completed "kwkF 5"
MCP robot send_command completed segment 1 "kwkF 5" ticket=1
```

机器人未返回 `k` 时，不会产生 MCP completed 日志，工具将失败或超时，模型不得回复动作已经完成。

## 验证

执行：

```bash
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
git diff --check
```

ESP32-S3 `AI_BOARD` 完整构建成功，固件输出到：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.0
```

格式检查脚本因环境中没有 `clang-format` 未执行，非源代码格式错误。

