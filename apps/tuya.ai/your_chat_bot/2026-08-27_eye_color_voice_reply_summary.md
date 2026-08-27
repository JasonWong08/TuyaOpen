# 2026-08-27 眼睛颜色语音控制与回复修复记录

## 目标

完善 `your_chat_bot` 的语音眼睛颜色控制，并解决以下问题：

- 用户说“把眼睛颜色调成粉色/紫色/红色”等指令时，机器人能通过 UART1 执行正确的 `vc*` 命令。
- 机器人描述自己的眼睛时使用第一人称“我的眼睛”，不再播报“你的眼睛”。
- 不把眼睛颜色描述为“眼部灯光”。
- 不把颜色错误换算成 `m0` 头部角度，例如紫色被错误调用为 `m0 135`。
- 眼睛颜色操作期间不播放“正在调整眼部灯光”等工具调用前的过渡语，只播放最终完成回复。

## 问题与根因

### 1. 云端缺少眼睛颜色命令信息

最初 MCP 工具只描述了身体动作和关节命令，没有明确列出眼睛颜色协议。云端模型曾把紫色色相错误理解为角度，调用：

```text
self.robot.send_command -> m0 135
```

但 `m0` 是机器人颈部/头部的物理转角命令，不控制颜色。

### 2. NLG 文本修改不能改变已经合成的 TTS

日志中的两种文本来自不同处理阶段：

```text
json-str ... 已经把你的眼睛调成紫色啦
text -> NLG ... 已经把我的眼睛调成紫色啦
```

- `json-str` 是云端返回的原始 NLG。
- `text -> NLG` 是固件执行应用级过滤后的文本。
- 实际语音是云端同时下发的已合成 TTS 音频。

因此，只把 NLG 字符串中的“你的”改成“我的”，可以修正文本事件和界面显示，却无法修改已经按原文合成的音频。

### 3. 云端偶发生成工具调用前的过渡语

云端有时会在 MCP 调用前生成：

```text
正在调整眼部灯光。
```

即使提示词要求使用“眼睛颜色”，生成式模型仍可能偶发不遵守。要确保实际播报稳定，必须在固件音频入口增加兜底屏蔽。

## 实现内容

### 1. 本地 ASR 识别眼睛颜色

在 `src/robot_uart_voice.c` 中增加眼睛目标、颜色和修改意图匹配。仅当语句同时包含眼睛目标、修改词和支持的颜色时才命中。

命令映射：

| 颜色 | UART 命令 |
| --- | --- |
| 红色 / red | `vcr` |
| 蓝色 / blue | `vcb` |
| 橙色 / orange | `vco` |
| 黄色 / yellow | `vcy` |
| 绿色 / green | `vcg` |
| 粉色 / pink | `vcp` |
| 紫色 / purple | `vcu` |

命中后沿用现有机器人仲裁链路，将命令以 `ASR` 来源加入队列并等待机器人返回 `v` 完成令牌。

### 2. 增加应用级 NLG 过滤钩子

在 `ai_components/ai_skills/src/ai_skill.c` 中增加弱符号：

```c
ai_app_filter_nlg_text(char *text, bool eof)
```

`your_chat_bot` 在 `robot_uart_voice.c` 中提供强符号实现：

- 仅在当前请求确认为眼睛颜色指令时，把“你的”替换为等长 UTF-8 文本“我的”。
- 如果 NLG 包含“眼部灯光”，将该 NLG 内容清空，不再向后续文本事件传递错误表述。
- NLG 结束时清除本次请求状态，避免影响普通聊天。

原始 `json-str` 日志仍可能显示云端原文，这是为了保留云端行为的诊断依据；过滤后的 `text -> NLG` 和用户侧文本不会继续使用错误表述。

### 3. 完善 MCP 工具描述和回复规则

在 `src/ai_mcp_robot_tools.c` 和 `include/robot_uart_ai_system_prompt.h` 中明确：

- 工具同时支持机器人身体动作和眼睛颜色修改。
- 列出全部 `vc*` 颜色命令。
- `m0` 只控制颈部/头部物理转角，绝不能控制颜色或色相。
- 眼睛颜色是机器人眼睛本身的颜色，不称为“眼部灯光”。
- 机器人使用第一人称描述自身，中文使用“我的眼睛”，不使用“你的眼睛”。
- 眼睛颜色请求应立即调用工具，调用前不生成进度播报；工具完成后再回复，例如“已经把我的眼睛调成红色啦”。

### 4. MCP 错误命令纠正与 ASR/MCP 去重

眼睛颜色 ASR 命中后会暂存本次 `vc*` 命令。若同一请求的 MCP 调用传来其他命令，MCP 入口会强制替换为暂存的眼睛颜色命令。

例如：

```text
MCP robot send_command: replace "m0 135" with pending eye-color command "vcu"
```

如果本地 ASR 已经发送相同的 `vcu`，现有 ASR/MCP 去重逻辑会复用 ASR ticket，不会重复执行动作，但 MCP 仍会等待并获得真实完成结果。

MCP 完成返回值还会再次要求云端最终回复使用“我的眼睛/我的眼睛颜色”，禁止“你的眼睛”和“眼部灯光”。

### 5. 屏蔽眼睛颜色操作的前置 TTS

在 `ai_components/ai_agent/src/ai_agent.c` 中增加弱符号：

```c
ai_app_should_suppress_tts_audio(void)
```

音频处理流程调整如下：

1. 中文眼睛颜色 ASR 命中后进入前置 TTS 屏蔽状态。
2. 如果云端音频流先到，不启动 TTS 播放器，并丢弃当前阶段的音频数据。
3. MCP 确认眼睛颜色命令完成后，将状态标记为已完成。
4. 下一段非空 NLG 被视为最终完成回复，解除屏蔽。
5. 后续音频数据到达时再启动 TTS 播放器，只播放最终回复。
6. 若云端没有 MCP 阶段，但最终回复包含“已经”，也会作为兼容条件解除屏蔽。
7. 聊天中断、音频流结束或 NLG EOF 时清理延迟播放状态。

典型诊断日志：

```text
ai agent: defer TTS while app suppresses preliminary audio
robot voice: removed incorrect eye-lighting NLG text
robot voice: allow completed eye-color TTS reply
ai agent: start TTS after suppressed preliminary audio
```

此机制只对中文眼睛颜色请求生效，不影响普通问答、身体动作和英文对话的 TTS。

## 修改文件

- `apps/tuya.ai/ai_components/ai_agent/src/ai_agent.c`
  - 增加应用级 TTS 屏蔽钩子。
  - 支持延迟启动 TTS 播放器和丢弃前置音频。
- `apps/tuya.ai/ai_components/ai_skills/src/ai_skill.c`
  - 增加应用级 NLG 文本过滤钩子。
- `apps/tuya.ai/your_chat_bot/src/robot_uart_voice.c`
  - 增加中英文眼睛颜色 ASR 规则。
  - 保存当前眼睛颜色命令和回复状态。
  - 实现第一人称修正、错误 NLG 清除和 TTS 屏蔽控制。
- `apps/tuya.ai/your_chat_bot/include/robot_uart_voice.h`
  - 声明眼睛颜色待执行命令查询和 MCP 完成通知接口。
- `apps/tuya.ai/your_chat_bot/src/ai_mcp_robot_tools.c`
  - 增加眼睛颜色工具协议。
  - 纠正当前眼睛颜色请求期间的错误 MCP 命令。
  - 返回眼睛颜色专用的完成回复约束。
- `apps/tuya.ai/your_chat_bot/include/robot_uart_ai_system_prompt.h`
  - 补充第一人称、颜色协议和禁止前置播报规则。

## 验证结果

### 增量编译

以下相关源码均已通过 ESP32 交叉编译并成功链接为 `libtuyaapp.a`：

```text
ai_agent.c
ai_skill.c
robot_uart_voice.c
ai_mcp_robot_tools.c
```

### 完整固件构建

执行：

```bash
tos.py build
```

ESP32-S3 `AI_BOARD` 完整构建成功。最终固件目录：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.0/
```

合并烧录固件：

```text
your_chat_bot_QIO_1.0.0.bin
```

`git diff --check` 通过。格式检查工具因当前环境缺少 `clang-format` 未执行完成，这不是源码编译错误。

## 建议回归场景

依次测试红、蓝、橙、黄、绿、粉、紫七种颜色，并重点确认：

1. UART1 发送对应的 `vc*` 命令。
2. MCP 不再把颜色转换成 `m0` 角度。
3. 相同 ASR/MCP 命令只执行一次。
4. 实际语音只播放最终完成句。
5. 最终回复使用“我的眼睛”，不出现“你的眼睛”或“眼部灯光”。
6. 普通聊天及坐下、前进、转头等身体动作的 TTS 播放不受影响。
