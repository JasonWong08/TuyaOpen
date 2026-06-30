# 2026-06-30 机器人控制问题修复总结

本文档记录今天围绕 `your_chat_bot` 项目中机器狗控制链路完成的排查、代码同步和修复内容。

## 1. 同步蓝牙手柄遥控逻辑

### 背景

当前项目中的蓝牙手柄遥控代码来自另一个 Arduino 项目：

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame
```

该 Arduino 项目后续有更新，需要将最新手柄映射和控制行为同步到当前 TuyaOpen 项目中。

### 处理内容

主要同步到以下文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_robot_bridge.c
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

同步内容包括：

- 更新左摇杆/方向控制对应的机器狗动作命令。
- 同步新增的特殊动作映射，例如 `dragWkF`、`flipRoll`、`tiptoeF`、`qDance` 等。
- 将 O 键切换命令从 `gb/gB` 更新为 `gu/gU`。
- 将 PLUS 长按触发时间从 1500ms 调整为 1000ms。
- 新增 ZL/LT 短按和长按动作：
  - 短按：`kqStepHalf`
  - 长按：`kqStep`
- 新增 R1 与左摇杆组合动作覆盖逻辑：
  - `kqBiped`
  - `kqScoot`
  - `kqFrontScoot`
  - `kglide`
- 更新 Q34B 手柄按键解析：
  - O/T 按普通 button bit 处理。
  - hat frame 活跃时冻结 Q34B button 状态，避免误触发。
  - 增加 raw/unknown 诊断日志，便于后续分析手柄报文。

## 2. 修复重复按同一个手柄按钮不重复执行动作

### 现象

上传固件后测试手柄操控功能时发现：

- 第一次按某个按钮，机器狗会执行动作。
- 重复按同一个按钮时，机器狗没有再次执行相同动作。

### 原因

问题出在 ABXY 按键释放分支。

当某个 solo 按键释放时，代码会 flush pending solo 并提前返回，但没有释放 `QUADDLE_NON_GAIT_HOLD_SOLO` 锁存状态。结果下一次按同一个按钮时，系统认为同类动作仍处于 hold 状态，从而抑制了重复发送。

### 修复

在 pending solo 被 flush 后，补充释放 non-gait hold：

```c
if (s_pending_solo_active && p[0] == s_pending_solo_btn) {
    OPERATE_RET rt = flush_pending_solo();
    non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
    return rt;
}

non_gait_hold_release(QUADDLE_NON_GAIT_HOLD_SOLO);
return OPRT_OK;
```

### 效果

重复按同一个手柄按钮时，机器狗可以重新执行同一个动作。

## 3. 修复语音后退命令仍发送 `kbk` 的问题

### 现象

将 `robot_uart_voice.c` 中语音规则里的 `kbk` 改成 `kbkF` 后，重新编译并上传固件，日志中仍然看到发送：

```text
kbk 5
```

典型日志：

```text
robot arbitration: AI command queued "kbk 5" from ASR
robot voice: ASR "再向后退五步。" -> queued UART "kbk 5"
second_uart: TX UART1 GPIO17/18 6 bytes: "kbk 5\n"
```

### 原因

问题不是单一规则表遗漏，而是存在多条路径：

1. `robot_uart_voice.c` 中动态步数解析会优先于 `s_voice_rules[]` 执行。
2. “再向后退五步”命中了 `__robot_voice_match_walk_cmd()` 中的后退短语。
3. 该动态路径里仍然硬编码生成：

```c
snprintf(cmd, cmd_len, "kbk %d", steps);
```

此外：

- `s_voice_rules[]` 中仍有部分短语 fallback 使用 `kbk 3`。
- MCP 工具描述仍写着 `kbk backward`，云端模型可能继续调用 `self.robot.send_command` 发送 `kbk 5`。
- `second_uart.c` 默认测试命令也仍是 `kbk 3`。

### 修复

涉及文件：

```text
apps/tuya.ai/your_chat_bot/src/robot_uart_voice.c
apps/tuya.ai/your_chat_bot/src/ai_mcp_robot_tools.c
apps/tuya.ai/your_chat_bot/src/second_uart.c
```

修复内容：

- 动态步数后退命令改为：

```c
snprintf(cmd, cmd_len, "kbkF %d", steps);
```

- 语音规则表中的后退 fallback 全部改为 `kbkF 3`。
- CLI 默认示例改为 `kbkF 3`。
- MCP 工具描述中将 backward 命令改为 `kbkF backward`。
- 在 MCP send_command 入口增加兼容归一化：

```c
if (strncmp(text, "kbk", 3) == 0 && (text[3] == '\0' || text[3] == ' ' || text[3] == '\t')) {
    snprintf(normalized_text, sizeof(normalized_text), "kbkF%s", text + 3);
    text = normalized_text;
}
```

这样即使云端仍发旧格式 `kbk 5`，固件侧也会自动转换为 `kbkF 5`。

## 4. 修复 ASR 与 MCP 重复发送同类动作

### 现象

语音命令可能同时走两条路径：

- 本地 ASR fallback 识别后直接排队机器人动作。
- 云端 MCP 工具随后又调用 `self.robot.send_command` 发送类似动作。

这会导致同一个意图短时间内执行两次，例如本地 ASR 发送一次后退，MCP 又发送一次后退。

### 设计目标

采用更稳的集中式做法：

- 当本地 ASR 已经识别并排队机器人动作后，短时间内忽略 MCP 发来的相同或同类动作。
- 去重逻辑放到 ASR/MCP 共用的机器人命令排队入口中，避免某一条调用路径遗漏。

### 修复

主要修改文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_robot_bridge.c
```

新增 5 秒去重窗口：

```c
#define AI_COMMAND_ASR_MCP_DEDUPE_MS 5000
```

新增最近 ASR 命令记录：

```c
static char     s_last_asr_cmd[QUADDLE_CMD_MAX];
static char     s_last_asr_cmd_head[16];
static uint32_t s_last_asr_cmd_ms;
```

核心策略：

- ASR 命令成功进入队列后，记录完整命令、命令头和时间戳。
- MCP 命令进入队列前，先检查最近 ASR 命令。
- 如果 5 秒内 MCP 命令与 ASR 命令完全相同，则丢弃。
- 如果 5 秒内 MCP 命令与 ASR 命令属于同类 `k*` 动作，例如：

```text
ASR: kbkF 5
MCP: kbkF 3
```

也会丢弃，避免同一个物理动作重复执行。

### 日志表现

当 MCP 命令被去重丢弃时，会输出：

```text
robot arbitration: MCP command "..." skipped; recent ASR command "..." already queued
```

### 边界行为

- 只对 `MCP` 来源做重复抑制。
- 不影响 ASR 本身排队。
- 不影响手柄优先级逻辑。
- 同类判断只对 `k*` 机器人动作放宽，不会把 `m0 45` 和 `m0 -45` 这类参数方向不同的命令误判为同类动作。

## 5. 构建验证

修改后执行过项目构建：

```bash
. ./export.sh && cd apps/tuya.ai/your_chat_bot && tos.py build
```

构建成功，输出目录：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.0
```

同时执行过：

```bash
git diff --check
```

结果通过。

格式检查 `tools/check_format.py` 未能执行完成，原因是当前环境缺少 `clang-format`，不是代码编译错误。

## 6. 当前结论

今天主要解决了三类实际控制问题：

1. 手柄控制逻辑与 Arduino 最新项目同步。
2. 重复按同一个手柄按钮不会重复执行动作的问题。
3. 语音/MCP 控制链路中后退命令旧格式 `kbk` 残留，以及 ASR/MCP 重复执行同类动作的问题。

修复后，手柄、本地 ASR、MCP 三条控制入口的行为更加一致，且机器人动作仲裁集中在 `quaddle_robot_bridge.c` 中处理，后续继续扩展控制路径时更容易维护。
