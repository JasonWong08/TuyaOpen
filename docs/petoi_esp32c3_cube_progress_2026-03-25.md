# PETOI_ESP32C3_CUBE 今日计划进度总结（2026-03-25）

## 一、计划对照（`docs/esp32c3项目收敛计划_d179d4de.plan.md`）

按计划主线“先稳连云与内存，再恢复语音/AI功能”，今日工作继续聚焦在阶段 B/C/D 的收口：

- 阶段 B（先稳配网与连云）：保持稳定，无回退。
- 阶段 C（白屏/板级冲突收敛）：保持已有修复结果，继续围绕音频与引脚冲突做最小变更。
- 阶段 D（提示音与AI能力恢复）：作为今日主任务推进，完成一轮“音频优先 + 板级声路”修复。

---

## 二、今日输入现象（来自最新实机日志）

在 `06126073` 版本基础上，日志表现为：

- 未再出现 `block_trim_free` / `Stack protection fault` 等崩溃。
- 仍出现 `audio player -> play alert type=3` 与 `play network-connected alert before ai_agent_init`。
- `codec-out` 已显示 `channel=1`、`pa_pin=13`，I2S 为 xiaozhi 映射。
- 设备在“添加成功后”只有短促响声，未听到完整联网提示音。
- 提示音触发窗口堆内存曾下探到约 `free=5564`，并出现 `Heap critical` 告警。

结论：问题已从“链路不通/崩溃”收敛为“音频播放窗口受内存与板级PA控制影响，导致仅瞬态响声”。

---

## 三、今日已实施变更（代码已提交并推送）

## 1) 音频播放窗口减负（低堆场景）

- 文件：`apps/tuya.ai/ai_components/ai_audio/src/ai_audio_input.c`
- 变更：
  - 为 `OVERFLOW_PSRAM_STOP_TYPE` 增加兼容回退宏，避免平台差异导致构建阻断。
  - 在 `AI_AUDIO_VAD_MANUAL` 模式下，缩小录音 ringbuf 缓存窗口（按 slice 计算最小窗口），降低提示音播放时的内存竞争。
- 目标：避免提示音触发时堆快速跌入临界区，提升完整播报概率。

## 2) 联网提示音时序前移（先播后切模式）

- 文件：`apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c`
- 变更：
  - MQTT connected 回调中，先播放 `AI_AUDIO_ALERT_NETWORK_CONNECTED`，再执行 `ai_mode_init(mode)`。
  - 增加短暂延时让音频链路先起播，减少与 recorder 模式切换的抢占。
- 目标：降低“提示音刚开始即被初始化扰动”的风险。

## 3) ES8311 的 PA 极性改为可配置（板级收敛关键）

- 文件：
  - `boards/ESP32/common/audio/tdd_audio_8311_codec.h`
  - `boards/ESP32/common/audio/tdd_audio_8311_codec.c`
  - `boards/ESP32/PETOI_ESP32C3_CUBE/board_config.h`
  - `boards/ESP32/PETOI_ESP32C3_CUBE/petoi_esp32c3_cube.c`
- 变更：
  - 新增 `pa_output_invert` 配置项（0=active-high，1=active-low）。
  - `EnableOutput()` 按板级配置控制 PA 使能电平。
  - PETOI 板默认设置 `AUDIO_CODEC_PA_INVERT=1`（active-low）。
- 目标：修复“仅短促响声（开关瞬态）”这一典型 PA 极性反相症状。

---

## 四、今日提交记录（按时间）

- `16219c3a` `fix(esp32c3-audio): reduce mic ringbuf and prioritize prompt startup`
- `5383bb7f` `chore: restore executable bit for ai_audio_input.c`
- `f7fd78bf` `fix(esp32c3-audio): add configurable PA polarity for ES8311 path`

以上提交均已推送到分支：

- `cursor/-bc-f540916f-0d05-42a7-82a0-8fb81f5a0dd1-dd02`

---

## 五、验证与当前阻塞

## 1) 已确认

- 代码变更已提交并推送。
- PR 已同步更新（#3）。
- 当前主线问题不再是崩溃，而是“提示音完整播报”稳定性。

## 2) 环境侧阻塞（非本次代码逻辑错误）

Cloud 构建阶段仍受平台分区配置影响：

- `partition table ... does not fit in configured flash size 4MB`
- 与 `platform/ESP32/tuya_open_sdk/partitions.csv` / flash size 设定相关

该问题属于构建环境配置项，不是本次音频逻辑改动引入。

---

## 六、明日刷机验收清单（建议按顺序）

刷入 `f7fd78bf` 后，重点看以下日志与现象：

1. 是否仍有：
   - `audio player -> play alert type=3`
   - `play network-connected alert before ai_agent_init`
2. `codec-out` 行是否为：
   - `channel=1`
   - `pa_pin=13`
3. 提示音触发点附近 heap 是否仍跌入极低值（如 `free~5KB`）。
4. 主观听感是否从“短促响声”变为“完整联网提示音”。
5. 是否继续保持“不崩溃”状态（无 `block_trim_free` / stack fault）。

---

## 七、当前阶段结论

今日工作已完成“音频优先”路径下的关键收口：  
**内存减负 + 播放时序优化 + PA 极性可配** 三项已落地。  
明日重点通过实机刷机验证“完整联网提示音”是否恢复；若恢复，则可进入阶段 E 的回归固化。

