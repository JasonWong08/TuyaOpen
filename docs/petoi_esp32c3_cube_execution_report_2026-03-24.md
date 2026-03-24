# PETOI_ESP32C3_CUBE 实施执行报告（2026-03-24）

参考文档：`docs/petoi_esp32c3_cube_architecture_summary.md`

## 1. 执行目标

按“平衡策略”推进以下收敛目标：

- 优先保证 BLE 配网与 TLS/MQTT 连通
- 缓解 ESP32-C3 无 PSRAM 场景的内存峰值问题
- 解决白屏相关的初始化时序问题
- 在连云稳定后再加载重模块（音频/AI）

---

## 2. 本轮已完成改动

### A. 基线观测与故障门槛（已落地）

文件：`apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c`

- 新增运行态统计：
  - `bind_start_count`、`mqtt_connected_count`、`mqtt_disconnected_count`
  - `bind_start_ms`、`mqtt_connected_ms`
  - 计算并打印 `Bind-to-MQTT latency`
- 增加关键内存门槛日志：
  - `free_heap < 8KB` 或 `largest_block < 4KB` 时打印 `Heap critical`
  - 明确提示 TLS/WiFi 可能因碎片化失败

### B. Cloud-first 主链路稳定（已落地）

文件：

- `apps/tuya.ai/petoi_esp32c3_cube/include/app_chat_bot.h`
- `apps/tuya.ai/petoi_esp32c3_cube/src/app_chat_bot.c`
- `apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c`
- `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`

核心改动：

1. 将应用初始化拆分为两阶段：
   - `app_chat_bot_precloud_init()`：仅做轻量 UI/状态和监控定时器
   - `app_chat_bot_postcloud_init()`：MQTT 成功后再做完整 AI/音频初始化
2. `tuya_main.c` 在 `TUYA_EVENT_MQTT_CONNECTED` 首次触发时执行 post-cloud init
3. 在 post-cloud 完成后再发布 `EVENT_MQTT_CONNECTED`，避免 AI 组件错过事件
4. 对无 PSRAM 的稳定化配置，显式关闭重模块：
   - `CONFIG_ENABLE_COMP_AI_VIDEO=n`
   - `CONFIG_ENABLE_COMP_AI_MCP=n`
   - `CONFIG_ENABLE_COMP_AI_PICTURE=n`

### C. 白屏与外设冲突收敛（已落地）

文件：

- `apps/tuya.ai/petoi_esp32c3_cube/src/app_chat_bot.c`
- `boards/ESP32/PETOI_ESP32C3_CUBE/board_config.h`

核心改动：

1. 在 pre-cloud 阶段提前拉起最小 UI（网络/状态/表情），避免“直到重模块初始化后才首帧渲染”
2. 针对运行日志中 `GPIO 12/13 is not usable`，调整 I2S 时钟引脚：
   - `I2S_MCK_IO: 13 -> -1`
   - `I2S_BCK_IO: 12 -> 11`

---

## 3. 构建验证结果

已执行（仓库根目录）：

```bash
. ./export.sh
cd apps/tuya.ai/petoi_esp32c3_cube
rm -rf .build
tos.py build
```

结果：**构建成功**（全量重建通过）。

并确认配置生效（`tuya_kconfig.h`）：

- `ENABLE_COMP_AI_MCP` 未设置
- `ENABLE_COMP_AI_VIDEO` 未设置
- `ENABLE_COMP_AI_PICTURE` 未设置

---

## 4. 与实施计划对应关系

- 阶段 A（基线观测）：完成
- 阶段 B（先打通连云）：完成（代码层）
- 阶段 C（白屏最小可用 UI + I2S 冲突修复）：完成（代码层）
- 阶段 D（提示音/AI 按阈值恢复）：完成第一版（post-cloud 延后恢复机制已具备）
- 阶段 E（回归封板）：完成构建回归，待硬件长稳验证

---

## 5. 硬件回归建议（下一步必做）

请在板端跑以下回归矩阵并记录日志：

1. 首次 BLE 配网（连续 3 次）
2. WiFi 断电重启重连（连续 10 次）
3. 弱网环境下激活重试（观察 TLS 是否仍 `mbedtls_ssl_setup` 失败）
4. 上电 30 分钟稳定性（观察 free/largest_block 趋势）
5. 屏幕首帧可见性（上电 5 秒内是否出现初始化文本）

判定标准：

- 不出现 `mbedtls_ssl_setup Fail 0xffff8100`
- 可以进入 `TUYA_EVENT_MQTT_CONNECTED`
- 屏幕不再全白且有基础状态文本

---

## 6. 风险与回退

- 若音频路径因 I2S 改脚导致异常，可先保留 cloud-first 架构并仅回退板级 I2S 引脚。
- 若堆仍跌到 `free < 8KB` 且 `largest < 4KB`，建议继续下调 AI 音频缓冲并延后更多非关键任务。

