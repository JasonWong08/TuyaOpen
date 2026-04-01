# Petoi ESP32-C3 今日问题处理总结（2026-03-31）

## 1. 今日目标

在 `petoi_esp32c3_cube` 上持续收敛以下问题：

1. 配网语音和 Boot 本地“哎”提示稳定；
2. 云端 AI 对话链路可用（不再反复超时）；
3. 避免极低堆导致的 TLS/MQTT 失败与日志卡死。

---

## 2. 今日实机现象（你提供的关键日志）

### 2.1 已经变好的部分

- 固件已确认运行到最新提交（先后看到 `41fb6a9c`、`a8a24775`）。
- 配网开始提示、配网成功提示正常。
- Boot 按键本地“哎”提示可播放。
- 未再出现之前的 `event or session id was null` 回归。

### 2.2 仍存在的核心问题

- 云链路仍反复失败：`tuya_ai_mqtt.c:154 mq ser cfg ack timeout -26113`。
- `ai_agent_init recovered by retry` 后，堆快速跌到极低（几百字节量级）：
  - `Heap: free=272`
  - `Heap: free=308`
- 同时出现大量 WiFi 侧异常分配日志：`wifi:m f null`。
- 结果是只能本地提示，云端说话内容不上行，看不到有效对话返回。

---

## 3. 今日已实施修复（按提交顺序）

## 3.1 提交 `a8a24775`

**主题**：修复 AI MQTT 应答等待路径 + 进一步压缩 C3 内存占用

### 改动点

1. `src/tuya_ai_service/svc_ai_basic/src/tuya_ai_mqtt.c`
   - 修正 `bizId` 匹配逻辑（避免错误比较导致误判）。
   - 在 server config/token 多分支下保证信号量唤醒，避免请求方无谓超时阻塞。
2. `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`
   - 继续下调 AI 输入/输出相关缓冲。
3. `src/tuya_cloud_service/netmgr/netmgr.c`
   - 小内存路径下优化 LAN 延迟门限逻辑（降低不必要干扰）。

### 结果

- 解决了一部分“等待链路卡死”的问题；
- 但在实机上仍存在 `ai_agent_init` 后堆被迅速打穿、`mq ser cfg ack timeout` 周期重现。

---

## 3.2 提交 `435a1b23`

**主题**：进一步降低云链路并发内存压力（当前最新）

### 改动点

1. `src/tuya_cloud_service/netmgr/netmgr.h`
2. `src/tuya_cloud_service/netmgr/netmgr.c`
   - 新增 `netmgr_stop_periodic_lan_init_timer()`；
   - 允许在不需要 LAN 控制的 SKU 上停止 500ms 周期 LAN 探测定时器。
3. `apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c`
   - `netmgr_init(type)` 后调用 `netmgr_stop_periodic_lan_init_timer()`；
   - 目标：减少 LAN 探测与 MQTT/TLS/AI 初始化并发抢堆。
4. `apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c`
   - `AI_AGENT_INIT_HEAP_MIN` 从 22KB 提升到 28KB；
   - 每个 MQTT 会话第一次 `ai_agent_init` 前增加 2.5s 错峰，避开 meta/TLS 高峰重叠。
5. `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`
   - `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM`：10 -> 8
   - `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM`：10 -> 8

### 结果

- 代码已完成构建验证（BUILD SUCCESS）；
- 需要你明天在硬件上验证该版本对 `mq ser cfg ack timeout`、`wifi:m f null`、极低堆的改善幅度。

---

## 4. 当前分支状态

- 当前分支：`cursor/-bc-f540916f-0d05-42a7-82a0-8fb81f5a0dd1-dd02`
- 当前最新修复提交：`435a1b23`
- 建议拉取命令：

```bash
git pull origin cursor/-bc-f540916f-0d05-42a7-82a0-8fb81f5a0dd1-dd02
```

---

## 5. 明日验证清单（按优先级）

## 5.1 版本确认

- [ ] 开机日志确认 `TuyaOpen commit-id: 435a1b23...`

## 5.2 云链路关键日志

- [ ] 是否出现：`netmgr: periodic LAN init timer stopped`
- [ ] 是否出现：`ai_agent: stagger 2.5s before first init`
- [ ] `mq ser cfg ack timeout -26113` 是否明显减少或消失
- [ ] `wifi:m f null` 是否显著减少
- [ ] `Heap: free=xxx` 是否不再跌到几百字节长期驻留

## 5.3 功能结果

- [ ] 配网开始/成功提示正常
- [ ] Boot “哎”提示正常
- [ ] 按下 Boot 后可看到语音上云与返回，不再只停留本地提示

---

## 6. 若仍失败，下一步计划（已预案）

若 `435a1b23` 仍不能稳定拉起云对话，下一步将优先尝试：

1. 把 `ai_audio_input` 的常驻初始化改为“按键触发时再启用”（延后占用）；
2. 进一步减少 AI 会话建立早期常驻内存（保持本地“哎”不回退）；
3. 按日志定向处理 `mq ser cfg` 超时时点对应的并发网络动作。

---

## 7. 备注

- 今天已经形成“本地提示稳定、云链路仍受极低堆影响”的明确边界；
- 明日验证重点不再是本地语音，而是“云链路在低堆下能否真正跑通并持续稳定”。
