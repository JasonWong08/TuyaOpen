# Free Talk 模式问题修复记录（2026-08-25）

## 1. 文档范围

本文记录 `apps/tuya.ai/your_chat_bot` 项目 Free Talk 模式在 2026-08-25 完成的问题分析、代码修改、固件验证结果和后续回归检查方法。

本次工作的最终目标如下：

- 连续 3 次空 ASR 或识别错误后，停止监听并退出 Free Talk 会话。
- 每次机器人语音回复结束并进入 `LISTEN` 后启动 10 秒定时器；10 秒无人说话则退出会话。
- 退出前播放明确的中英文告别语。
- 告别语结束后发送 `ved`，退出表情特效并恢复默认表情。
- 进入 `IDLE` 后重新启用 KWS，后续必须使用唤醒词唤醒。
- 英语对话使用英语告别语，中文对话使用中文告别语。
- 修复云端 NLG 文本中残留的字面量 `\u0020`。
- 避免云端 Agent 在同一次退出请求中生成两遍告别语。

## 2. 最终状态流程

正常一轮对话结束后的状态流程为：

```text
SPEAK 完成
  -> LISTEN
  -> 启动 10 秒无人说话定时器
  -> 超时，或累计 3 次空 ASR/ASR_ERROR
  -> 停止麦克风上传和 VAD 监听
  -> THINK（发送固定告别提示词）
  -> SPEAK（播放中/英文告别语）
  -> 播放完成
  -> 发送 ved
  -> IDLE
  -> 重新启用 KWS
```

告别请求还设置了 30 秒保护定时器。如果云端请求失败、返回错误、TTS 中止或长时间没有完成，系统仍会结束退出流程，避免永久停留在 `THINK`/`SPEAK`。

## 3. 问题和修改过程

### 3.1 连续空 ASR 的退出门槛

需求：连续 3 次收到空 ASR 或 ASR 识别错误时提前退出监听。

修改：

- 将 `AI_CHAT_EMPTY_IDLE_LIMIT` 设置为 `3`。
- `AI_USER_EVT_ASR_EMPTY` 和 `AI_USER_EVT_ASR_ERROR` 使用同一个累计计数。
- 正常 ASR 成功后清零计数。
- 前两次空 ASR 会重新进入 `LISTEN` 并重新启动监听；第 3 次进入统一告别退出流程。

相关代码：

- `apps/tuya.ai/ai_components/ai_mode/src/ai_mode_free.c`

### 3.2 LISTEN 状态 10 秒无人说话超时

需求：机器人语音回复完成后，每次进入 `LISTEN` 都启动 10 秒定时器；无人说话时退出。

修改：

- 新增并使用 `AI_CHAT_LISTEN_TIMEOUT_MS = 10 * 1000`。
- 将等待云端回复的保护时间与监听时间拆开：
  - LISTEN 超时：10 秒。
  - THINK/告别请求保护超时：30 秒。
- TTS 播放完成后快速恢复录音，避免等待过久剪掉用户短句开头。
- 空 ASR 后重新进入 `LISTEN` 时重新启动 10 秒计时。

最新英文测试中，最后一次空 ASR 出现在 `16:00:55`，退出日志出现在 `16:01:05`，符合 10 秒要求。

### 3.3 统一告别、IDLE、KWS 和 ved 收尾

新增统一的退出过程：

- `__ai_mode_free_begin_exit()`：停止监听、关闭唤醒输入、选择告别语言并发送告别请求。
- `__ai_mode_free_complete_exit()`：清理退出状态、发送应用退出钩子、进入 `IDLE`。
- `ai_app_on_free_mode_exit()`：弱符号应用钩子。
- `your_chat_bot` 中实现强符号版本，发送 UART 指令 `ved` 并清理 `s_expect_vet`，避免随后到达的 `PLAY_END` 又发送 `vet` 覆盖 `ved`。

退出流程可由以下情况进入：

- LISTEN 10 秒超时。
- 连续 3 次空 ASR/ASR 错误。
- 云端发出退出事件。
- 应用识别到明确退出短语并调用 `ai_mode_free_request_exit()`。

涉及文件：

- `apps/tuya.ai/ai_components/ai_mode/src/ai_mode_free.c`
- `apps/tuya.ai/ai_components/utility/include/ai_user_event.h`
- `apps/tuya.ai/ai_components/utility/src/ai_user_event.c`
- `apps/tuya.ai/your_chat_bot/src/robot_uart_voice.c`

### 3.4 告别语没有播放的问题

早期固件的日志显示系统进入退出流程并发送了 `ved`，但没有听到告别语。

根因：

- `__ai_mode_free_begin_exit()` 先停止原有音频输入，然后通过 `ai_agent_send_text()` 启动新的告别文本请求。
- 状态切换到 `THINK` 后，`__ai_mode_enter_think()` 再次无条件调用 `__ai_mode_free_stop_active_input()`。
- 第二次停止操作误把刚启动的告别文本请求取消，因此云端没有机会返回完整 TTS。

修复：

```c
if (!sg_goodbye_pending) {
    __ai_mode_free_stop_active_input();
}
```

告别请求进行中时，不再由 `THINK` 入口重复停止新的文本输入会话。

后续固件日志已确认：云端能够返回告别 NLG，播放器出现 `FG eof`，然后发送 `ved` 并恢复 KWS。

### 3.5 中英文告别语自动选择

中文告别目标：

```text
好的，如果你没有什么想聊的话题或者要求，我们下次再聊。
```

英文告别目标：

```text
Okay, if there is nothing else you would like to talk about or ask, let's chat again next time.
```

注意：中文提示词本身不会可靠地“自动翻译”为英文。因此代码中保留独立的中英文提示词，而不是依赖模型翻译。

语言选择规则：

- 每次收到成功 ASR 时检查文本。
- 包含 CJK 中文字符时记录中文上下文。
- 没有中文字符但包含 ASCII 英文字母时记录英文上下文。
- 如果编译配置默认启用英语，则初始默认值为英语；否则默认为中文。
- 最终退出时使用最近一次有效 ASR 所确定的语言。

最新英文测试中，告别内容正确为英文。

### 3.6 `\u0020` 字面量问题

现象：原始云端日志包含：

```json
{"content":"\\u0020I'm doing great,"}
```

cJSON 完成外层解析后，`content` 中仍可能残留六个普通字符 `\u0020`。这是云端内容被二次 JSON 转义造成的；`U+0020` 实际表示普通空格。

修复：

- 在 NLG 文本进入 UI/事件分发之前，原地解码残留的 `\uXXXX`。
- 支持普通 BMP Unicode 字符。
- 支持 UTF-16 高低代理对，可正确还原 Emoji 等非 BMP 字符。
- 正常 UTF-8 内容保持不变。
- 非法转义和 `\u0000` 保持原样，避免引入字符串截断。
- 只处理 NLG 内容，不处理 ASR，防止改变用户真实识别文本。

涉及文件：

- `apps/tuya.ai/ai_components/ai_skills/src/ai_skill.c`

验证结果：最新日志的原始 `json-str` 仍会显示 `\\u0020`，这是预期的解码前记录；后续 `text -> NLG` 已显示为普通前导空格，不再出现字面量 `\u0020`。

### 3.7 告别语播放两遍

问题日志的关键时间线：

```text
15:33:22  本地只执行一次 mode free announce exit before idle
15:33:23  云端一次性返回完整英文告别句，timeIndex=24350
15:33:27  同一会话再次从 "Okay," 开始返回相同句子，timeIndex=1450
15:33:32  第二遍之后才 finish=true
15:33:33  只有一次播放器 FG eof、ved 和 KWS 恢复
```

结论：

- 不是 10 秒定时器触发两次。
- 不是本地调用了两次 `ai_agent_send_text()`。
- 不是播放器把一个已完成文件重新播放。
- 同一个云端 Agent 请求产生了“前置回复”和“最终回复”两个阶段；固定告别提示词使两个阶段都生成了相同句子，并进入同一 TTS 输出过程。

修复策略仅在 `sg_goodbye_pending` 时启用：

- 从 NLG JSON 正确读取 `timeIndex`。
- 保存本次告别 NLG 最近的有效时间位置。
- 如果时间位置明显回退，并且新文本又从当前语言的告别语开头（`Okay`/`好的`）开始，则判定为重复告别阶段。
- 立即发送 `AI_EVENT_CHAT_BREAK`、停止前景 TTS，并完成 `IDLE`/`ved`/KWS 收尾。
- 正常聊天和机器人动作允许存在动作前、动作后两段回复，不应用此去重规则。

同时修改播放器停止逻辑：停止前景 TTS 或全部播放器时，立即清除 `__s_tts_play_flag`。云端在停止后迟到的音频包会被丢弃，不会再次续播。

预期命中日志：

```text
mode free suppress duplicate goodbye phase, timeIndex 24350 -> 1450
```

最新测试中云端只生成了一轮告别语，因此没有命中这条保护日志；实际听感和退出行为均为单次告别。保护分支需要在云端未来再次产生重复阶段时继续验证。

### 3.8 NLG `timeindex` 随机值

旧代码只给 `AI_NOTIFY_TEXT_T` 的 `data` 和 `datalen` 赋值，没有初始化 `timeindex`，日志中曾出现类似 `1011267012` 的随机栈数据。

修复：

- 使用 `{0}` 初始化 `AI_NOTIFY_TEXT_T`。
- 从 NLG JSON 的 `timeIndex` 字段读取真实值。
- 日志使用无符号格式输出。

最新日志已显示真实值，例如 `450`、`4750` 和 `15700`。

注意：云端 `timeIndex` 在同一组普通 NLG 分片内也可能回退，例如 `450 -> 4750 -> 3750`。因此去重不能只根据时间回退判断，代码还同时要求处于告别流程并重新匹配告别语开头，以避免误伤正常回答。

## 4. 文件修改清单

### `apps/tuya.ai/ai_components/ai_mode/src/ai_mode_free.c`

- 空 ASR/错误退出门槛改为 3 次。
- LISTEN 超时设置为 10 秒。
- THINK/告别保护超时设置为 30 秒。
- 新增统一告别开始和完成函数。
- 增加中英文上下文检测及告别提示词选择。
- 修复告别文本请求被 THINK 状态再次停止的问题。
- 增加告别 NLG 二阶段去重。
- 播放结束、错误、超时等路径统一完成 IDLE/KWS 收尾。

### `apps/tuya.ai/ai_components/ai_skills/src/ai_skill.c`

- ASR 结果继续通过应用钩子交给机器人 UART 命令匹配。
- 增加二次转义 Unicode 字面量解码。
- 正确初始化并解析 NLG `timeIndex`。

### `apps/tuya.ai/ai_components/ai_audio/src/ai_audio_player.c`

- 主动停止前景或全部播放器时立即关闭 TTS 流接收标志。
- 防止退出后迟到的云端音频数据继续进入播放器。

### `apps/tuya.ai/ai_components/utility/include/ai_user_event.h`

- 声明录音开始应用钩子。
- 声明 Free Talk 退出应用钩子 `ai_app_on_free_mode_exit()`。

### `apps/tuya.ai/ai_components/utility/src/ai_user_event.c`

- 提供上述应用钩子的弱符号默认实现。

### `apps/tuya.ai/your_chat_bot/src/robot_uart_voice.c`

- 录音实际开始时发送 `vetL`。
- Free Talk 完成退出时发送 `ved`。
- 清理 `s_expect_vet`，避免退出后的播放结束事件发送多余 `vet`。
- ASR 命中退出短语时调用 Free Talk 退出请求。

`apps/tuya.ai/your_chat_bot/include/tuya_config.h` 中可能因构建工具自动选择有效凭据而出现注释位置变化；这不是本次功能逻辑修改，本文不记录或展示任何设备凭据。

## 5. 构建和验证

构建命令：

```bash
. ./export.sh
cmake --build apps/tuya.ai/your_chat_bot/.build --target example
```

完整 ESP32S3 构建、链接、分区检查和合并固件均成功。

当前合并固件位置：

```text
platform/ESP32/tuya_open_sdk/build/your_chat_bot_QIO_1.0.0.bin
```

代码差异检查：

```bash
git diff --check
```

检查通过。格式检查脚本尝试执行，但当前环境没有安装 `clang-format`，因此未完成自动格式检查：

```text
Error: clang-format is not installed or not in PATH
```

## 6. 最新英文测试结论

最新测试时间线：

```text
16:00:55  空 ASR，重新进入 LISTEN
16:01:05  10 秒后触发一次告别退出
16:01:07  英文告别从 Okay, 开始
16:01:17  NLG/TTS 完成
16:01:17  发送一次 ved
16:01:17  KWS 重新启用
```

已确认：

- 英语对话使用英语告别语。
- 10 秒 LISTEN 超时正确。
- 告别语只播放一遍。
- `ved` 只发送一次。
- 最终进入 IDLE 并恢复 KWS。
- `\u0020` 已在 NLG 层正确还原为空格。
- `timeIndex` 日志已修复。

## 7. 已知非阻塞观察项

### 音频输入偶发拥塞

日志曾出现一次：

```text
audio input congested, drop frame after 200 ms
```

后续 ASR 成功，系统已自行恢复。若出现频率升高或造成语音首字丢失，需要单独检查输入环形缓冲区、上传带宽和任务调度延迟。

### 未配置的 Emoji 表情

日志出现：

```text
not found emoji: U+1F91D return NEUTRAL as default
```

当前会回退为 `NEUTRAL`，不影响语音、动作和退出流程。后续如需握手表情，可在 emotion 映射中补充 `U+1F91D`。

### 云端重复告别保护仍需机会性验证

最新固件测试时云端没有再次生成两阶段重复告别，因此实际保护分支尚未在最新固件日志中命中。未来若再次出现云端重复，应确认：

```text
mode free suppress duplicate goodbye phase
```

出现后第二遍音频应立即被抑制，随后只发送一次 `ved` 并恢复 KWS。

## 8. 建议回归测试清单

- 中文对话一轮后保持安静 10 秒：中文告别一次、`ved` 一次、KWS 恢复。
- 英文对话一轮后保持安静 10 秒：英文告别一次、`ved` 一次、KWS 恢复。
- 连续制造 3 次空 ASR：第 3 次后告别并退出。
- 空 ASR 1～2 次后正常说话：计数清零，对话继续。
- 云端断网或告别请求无响应：30 秒保护超时后进入 IDLE/KWS。
- 告别 TTS 中止或报错：立即完成 `ved`、IDLE、KWS 收尾。
- 检查原始 NLG 包含 `\\u0020` 时，应用层文本不再显示字面量 `\u0020`。
- 动作命令存在动作前/动作后回复时，正常回复不得被告别去重逻辑误杀。
- 退出后再次说话不应直接唤醒，必须使用配置的 KWS 唤醒词。

