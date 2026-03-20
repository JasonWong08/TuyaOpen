# PETOI ESP32-C3 今日问题简报（2026-03-20）

## 一句话结论

今天已把系统从“启动期频繁 OOM/崩溃 + BLE 配网不稳定 + 屏幕异常”收敛到“BLE 可稳定完成配网数据交换”，但仍存在 **WiFi 关联阶段失败（reason 4/204）** 与 **白屏偶发**，下一步继续围绕关联稳定性和内存水位优化。

## 今日已完成的关键修复

- **内存大户静态化**：MP3 解码 scratch 和解码上下文改为静态内存，降低运行期大块 `malloc` 失败概率。  
- **KV/LFS 降内存**：LittleFS cache/read/prog 颗粒度下调，缓解 `lfs_file_rawopencfg` 申请失败。  
- **配网模式收敛**：在 C3 无 PSRAM 机型上改为 BLE 配网，避免 SoftAP+BLE 共存崩溃。  
- **BLE 稳定性优化**：广播更新加去重与回退机制；配网参数确认后主动断开 BLE 连接，降低 WiFi 关联阶段共存压力。  
- **LVGL 降压与兜底**：`LV_DRAW_LAYER_SIMPLE_BUF_SIZE` 24KB→4KB；状态栏去滚动；字体失败增加 fallback（`LV_FONT_DEFAULT`/`:)`/`Initializing...`）；首帧强制重绘。  
- **显示映射修正**：`DISPLAY_HEIGHT=280`，`DISPLAY_ST7789_Y_GAP=20`，保持 partial buffer（不启 full refresh）。  
- **任务栈与缓冲缩减**：多任务栈和 `AI_PLAYER` 缓冲继续下调，释放可用堆。  
- **配置生效问题修复**：WiFi/BLE pool 参数已强制写入并验证生效（`rx/tx=10`, `mgmt_sbuf=20`）。

## 当前状态（基于最新日志）

- 正向进展：  
  - BLE 可扫描、可连接、可收发配网数据（SSID/密码/token）；  
  - 绑定提示音在低堆下已自动跳过，避免早期 OOM。  
- 仍未闭环：  
  - WiFi 在关联阶段反复失败（`Disconnect reason: 4/204`）；  
  - 堆水位仍偏低（运行中可降到几百字节级）；  
  - 屏幕仍有白屏场景（需继续验证新兜底是否稳定触发）。

## 明日优先动作（建议）

1. 全量 clean + build + flash，确认本轮代码全部生效。  
2. 重点抓三类日志：  
   - BLE 配网完成后是否立即断链；  
   - WiFi 关联失败 reason 是否收敛；  
   - `free/largest_block/min_ever` 是否保持在安全区。  
3. 若 reason 4/204 持续：优先从 WiFi 关联参数与共存策略继续收敛（并保持当前内存优化不回退）。

## 风险提示

- ESP32-C3 无 PSRAM 平台资源极限明显，任何新增功能（动画、音频、缓冲、并发任务）都可能引发回归；建议后续改动默认先做“内存预算评估 + 日志基线对比”。
