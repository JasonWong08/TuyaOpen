# PETOI ESP32-C3（无 PSRAM）今日问题处理总结（2026-03-20）

## 背景

目标平台为 `petoi_esp32c3_cube`（ESP32-C3，无 PSRAM）。  
今日主要围绕以下两类问题持续排查与修复：

- 运行期内存不足（OOM）导致 BLE/LVGL/音频异常；
- 屏幕显示异常（雪花、白屏、元素缺失）与 BLE 配网失败。

---

## 一、今日已定位并修复的问题

### 1) MP3 解码阶段大块内存申请失败

**现象**
- `mp3dec_decode_frame` / `decoder_mp3_start` 发生 `malloc failed`；
- 绑定提示音或音频初始化阶段容易触发 OOM。

**根因**
- 无 PSRAM 场景下，MP3 解码临时缓冲和解码上下文占用大块连续堆；
- 堆碎片化后更容易失败。

**修复**
- `src/audio_player/src/decoder/minimp3/minimp3.h`  
  启用 `MINIMP3_USE_STATIC_SCRATCH`，将帧级 scratch 改为静态内存。
- `src/audio_player/src/decoder/decoder_mp3.c`  
  无 PSRAM 时将 `DECODER_MP3_CTX_T` 改为 BSS 静态单例。

---

### 2) LittleFS/KV 在 4KB 缓存配置下申请失败

**现象**
- `lfs_file_rawopencfg` 报 `malloc failed:0x1000`。

**根因**
- 默认 cache/read/prog 粒度偏大，对连续块要求高。

**修复**
- `src/tal_kv/src/tal_kv.c`  
  在大块分区场景将 `cache_size/read_size/prog_size` 动态下调（典型 1024/256/256），降低连续内存需求。

---

### 3) SoftAP + BLE 共存触发崩溃

**现象**
- WiFi 驱动报错后崩溃（如 `wifi:alloc eb ... fail`）。

**根因**
- ESP32-C3 无 PSRAM 下，SoftAP 与 BLE 并发内存压力过高。

**修复**
- `apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c`  
  配网模式改为仅 BLE（禁用 AP 配网）。

---

### 4) BLE 配网阶段内存与广播稳定性问题

**现象**
- `BLE_INIT: Malloc failed`、`ble packet len err`；
- 配网过程广播更新不稳定，偶发“能扫描但流程失败”。

**根因**
- BLE 编码临时缓冲在堆上申请；
- 广播更新存在频繁/冗余操作，低内存下更脆弱。

**修复**
- `src/tuya_cloud_service/ble/ble_mgr.c`
  - 编码帧缓冲改为 BSS 静态数组；
  - 引入 `adv_started` + 上次 payload 快照，避免无变化时重复更新；
  - 更新失败回退 `stop/set/start`。
- `src/tuya_cloud_service/ble/ble_netcfg.c`
  - BLE 收到并确认配网参数后，主动断开当前 BLE 连接，降低 WiFi 关联阶段 BLE 共存压力。
- `src/tuya_cloud_service/ble/ble_mgr.h/.c`
  - 新增 `tuya_ble_disconnect_current()` 对外接口用于上述流程。

---

### 5) LVGL 显示层导致堆压力与碎片化

**现象**
- 屏幕雪花、白屏、元素缺失；
- `largest_block` 快速下降，后续 BLE/WiFi 操作失败概率增大。

**根因**
- 旧清屏策略在 SPI 事务路径上引入额外碎片化；
- `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` 过大（24KB）导致层缓冲分配“贪婪”；
- 64px emoji 图元渲染临时需求过大（RGB565A8 路径）。

**修复**
- `boards/ESP32/common/display/lv_port_disp.c`  
  移除逐行手动清屏逻辑，避免事务分配抖动。
- `boards/ESP32/common/display/lv_vendor.c`  
  初始化后触发首次失效重绘，使用 LVGL 正常路径清屏。
- `src/liblvgl/v9/conf/lv_conf.h`
  - `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` 从 24KB 降为 4KB；
  - `LV_PNG_USE_PSRAM` 根据 `ENABLE_EXT_RAM` 条件启用。
- `apps/tuya.ai/ai_components/ai_ui/src/ai_ui_chat_chatbot.c`
  - 状态栏长文本改为 `LV_LABEL_LONG_CLIP`（减少持续重绘压力）；
  - 字体加载失败时增加可见 fallback（`LV_FONT_DEFAULT` / `:)` / `Initializing...`）；
  - 完成 UI 构建后主动 `invalidate` 首帧重绘。

---

### 6) ST7789 显示映射不匹配（雪花段/错位）

**现象**
- 靠排线侧出现雪花段；
- 顶部被裁切或底部显示异常。

**根因**
- 240x280 物理面板映射到 ST7789 240x320 GRAM，需要 Y 偏移；
- 分辨率与偏移配置不一致。

**修复**
- `boards/ESP32/PETOI_ESP32C3_CUBE/board_config.h`
  - `DISPLAY_HEIGHT` 调整为 `280`；
  - `DISPLAY_ST7789_Y_GAP` 设置为 `20`；
  - 保持 partial buffer（`DISPLAY_LVGL_FULL_REFRESH=0`）避免 full-buffer 断言。

---

### 7) 绑定阶段语音提示挤占最后可用堆

**现象**
- `TUYA_EVENT_BIND_START` 后堆快速见底，BLE/LVGL 后续失败。

**根因**
- 绑定提示音触发时机与系统高峰内存重叠。

**修复**
- `apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c`  
  在 `BIND_START` 时先检查 free heap，低于阈值（16KB）跳过提示音（日志已可见 `skip bind alert due low heap`）。

---

### 8) 任务栈/缓冲超预算（持续挤压可用堆）

**现象**
- `Phase-4` 后仅剩约 9~10KB，运行中进一步跌到 2KB 甚至几百字节。

**修复（本轮累计）**
- `apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c`  
  `ai_chat_mode` 任务栈进一步下调至 2KB。
- `apps/tuya.ai/ai_components/ai_ui/src/ai_ui_manage.c`  
  `ai_ui` 任务栈下调至 3KB。
- `boards/ESP32/common/audio/tdd_audio_8311_codec.c`  
  `esp32_i2s_8311_read` 栈下调至 2KB。
- `apps/tuya.ai/ai_components/ai_audio/src/ai_audio_input.c`  
  `record_task` 栈下调至 2KB。
- `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`
  - `CONFIG_AI_PLAYER_RINGBUF_SIZE=2048`
  - `CONFIG_AI_PLAYER_STACK_SIZE=2048`
  - 保持输入/输出 ringbuf 与写 socket 缓冲在低内存档位。

---

### 9) WiFi/BLE 驱动池参数未生效导致“改了但运行不变”

**现象**
- 日志仍显示旧参数（如 rx buffer 16）。

**根因**
- 未彻底清理构建缓存，旧 `sdkconfig` 覆盖默认项。

**修复**
- 同步修改并强制低内存参数到：
  - `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`
  - `platform/ESP32/tuya_open_sdk/sdkconfig`
  - `platform/ESP32/tuya_open_sdk/sdkconfig.defaults`
- 要求完整清理后重编译，后续日志已确认 `dynamic rx/tx buffer num: 10`、`mgmt sbuf: 20` 生效。

---

## 二、当前状态（基于最新日志）

### 已确认改善
- 固件确实为新版本（编译时间已更新）；
- BLE 可扫描、可连接、可接收并解析配网参数（SSID/密码/token）；
- 绑定提示音已按低堆阈值跳过（避免早期 OOM）。

### 仍需继续验证/收敛
- WiFi 关联仍出现反复失败（`Disconnect reason : 4 / 204`）；
- 屏幕仍可能出现白屏（虽已加字体兜底与强制首帧重绘）；
- 运行期堆仍偏紧（日志可见低位徘徊）。

---

## 三、建议的验证步骤（下一轮）

1. 全量清理并编译（确保所有配置与代码生效）：

```bash
cd /home/jasonw/Projects/TyOpen_Jason/TuyaOpen
. ./export.sh
cd apps/tuya.ai/petoi_esp32c3_cube
rm -rf .build
tos.py build
```

2. 烧录后重点核对日志：
- 是否出现 `skip bind alert due low heap: ...`；
- BLE 下发配网参数后是否立即执行 BLE 断链（新逻辑）；
- WiFi 关联失败的 reason 码是否收敛；
- `Heap free / largest_block` 是否不再跌到极低值。

3. UI 观察点：
- 是否出现 `Initializing...`（字体兜底文本）；
- emoji 不可用时是否至少显示 `:)`；
- 白屏是否改善为可见基础 UI。

---

## 四、关键结论

今日核心成果是把问题从“早期 OOM + BLE 初始化失败 + 显示错位/雪花”收敛到“可稳定进入 BLE 配网数据交换阶段”，并通过多处内存治理与显示兜底机制显著降低了崩溃概率。  
当前剩余主要矛盾是 **WiFi 关联阶段（reason 4/204）与超低堆余量并发**，下一轮应优先围绕 WiFi 关联稳定性和运行期内存水位继续收敛。
