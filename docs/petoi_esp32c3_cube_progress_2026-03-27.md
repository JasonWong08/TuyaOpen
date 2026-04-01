# PETOI_ESP32C3_CUBE 今日问题解决总结（2026-03-27）

## 一、今日目标

围绕 `docs/esp32c3项目收敛计划_d179d4de.plan.md` 的“语音优先”主线，今天集中解决两类问题：

1. 联网后可播报提示音，但程序反复 crash。
2. 不再 crash 后，仍无法进入 AI 云端问答（按 Boot 仅有本地提示音）。

---

## 二、今日实机现象（来自你提供的最新日志）

### 阶段 A：可播报提示音，但反复崩溃

- 设备联网后可听到“我已经联网，让我们开始对话吧”。
- 随后反复出现：
  - `Guru Meditation Error: Core 0 panic'ed (Stack protection fault)`
  - `Detected in task "ai_chat_mode"`
  - 回溯落在 `_svfprintf_r` / `snprintf`

### 阶段 B：崩溃消失，但仍无问答回复

- `ai_chat_mode` 栈崩溃已消失。
- 按 Boot 可听到“哎”一声（本地提示链路正常）。
- 但仍无云端问答回复，日志出现：
  - `defer ai_agent_init due low/fragmented heap: free=10376 largest=6144 need_free>=18432 need_largest>=8192`
  - `event or session id was null`
- 说明 `ai_agent_init` 长期被门限挡住，会话未建立，导致只剩本地提示，不进入云端对话。

---

## 三、今日代码修复内容

## 1) 修复 `ai_chat_mode` 任务栈溢出（稳定性）

- 文件：`apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c`
- 改动：
  - 新增 `AI_CHAT_MODE_TASK_STACK_SIZE`，在 ESP32 平台使用 `6144`（原来 2048）。
  - 将 `ai_chat_mode` 线程创建时的 `stackDepth` 改为该宏。
  - 将重试成功日志改为轻量文本，减少格式化路径栈压力峰值。
- 目标：消除 `ai_chat_mode` 的 `Stack protection fault`。

对应提交：
- `14467c4c` `fix(c3): increase ai_chat_mode stack to prevent retry-path overflow`

## 2) 修复“无问答回复”：让 `ai_agent_init` 在低堆区间仍可重试

- 文件：`apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c`
- 改动：
  - 将 C3 重试门限从 `18KB/8KB` 调整为 `10KB/6KB`（与当前实机运行区间对齐）。
  - 增加 `sg_ai_agent_retry_now`：
    - MQTT 回调 defer/失败时，立即标记“尽快重试”。
    - 用户按键开始对话时，也触发一次“即时重试”。
  - 重试逻辑增加播放期避让：
    - `ai_audio_player_is_playing()` 时不抢占初始化，播放后再试，保留“提示音优先”策略。
  - 保持 MQTT 断线状态复位逻辑。
- 目标：避免 `ai_agent_init` 永久卡在门限外，恢复云端会话建立机会。

对应提交：
- `603e92ec` `fix(c3): unblock ai_agent retry under low heap and button trigger`

---

## 四、今日提交清单（按时间）

- `c6ca2be2` `fix(c3): retry ai_agent init after low-heap defer`
- `14467c4c` `fix(c3): increase ai_chat_mode stack to prevent retry-path overflow`
- `603e92ec` `fix(c3): unblock ai_agent retry under low heap and button trigger`

以上提交均已推送到分支：
- `cursor/-bc-f540916f-0d05-42a7-82a0-8fb81f5a0dd1-dd02`

---

## 五、今天的构建验证结果

每轮关键改动后均执行了 `petoi_esp32c3_cube` 构建验证，结果均为成功：

- 产物目录：
  - `apps/tuya.ai/petoi_esp32c3_cube/dist/petoi_esp32c3_cube_1.0.0/`
- 固件文件：
  - `petoi_esp32c3_cube_QIO_1.0.0.bin`

说明：代码修改已达到“可编译、可刷机”的交付状态。

---

## 六、当前状态判断

1. **稳定性维度**：  
   `ai_chat_mode` 栈崩溃已针对性修复，日志已不再复现该 crash。

2. **功能维度（AI问答）**：  
   当前重点瓶颈已从“崩溃”转为“低堆下 agent 初始化窗口狭窄”。  
   今日已通过“放宽门限 + 即时重试 + 播放期避让”完成一轮恢复性修复。

3. **板级音频维度**：  
   仍可见 `GPIO 13/12 is not usable` 告警，说明板级 I2S 引脚冲突风险仍在；这属于后续音质/可靠性优化项，不是今天“对话链路阻塞”的主因。

---

## 七、你后续上传验证建议（本轮最终验收）

请用本次最新代码刷机后重点观察以下 5 点：

1. 是否不再出现 `task "ai_chat_mode"` 的 `Stack protection fault`。
2. MQTT 后是否仍有 `defer ai_agent_init ...`，以及随后是否出现重试成功迹象（如 `ai_agent_init recovered by retry`）。
3. 按 Boot 说话后，是否仍出现 `event or session id was null`（理想是减少或消失）。
4. 是否能收到云端语音回复（不仅“哎”提示）。
5. 若仍无回复，记录当时 `free` 与 `largest`，用于下一步精确收敛阈值或继续减载。

---

## 八、阶段结论

今天已完成“最后一轮可上传验证”的代码收口：  
**先稳住崩溃，再打通低堆问答初始化路径**。  
当前版本已具备你要求的“最后一次修改后可上传验证”条件。

