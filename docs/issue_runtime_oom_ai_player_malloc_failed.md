# 问题：运行时 OOM（`tuya_ai_player_create` malloc 失败）

## 现象

程序启动后 AI 管道初始化阶段 `tuya_ai_player_create` 分配内存失败，AI 功能无法使用：

```
[01-01 00:00:01 ty E][tal_system.c:96] 0x4200daa8 malloc failed:0x2000 free:0x21dc
--- 0x4200daa8: tuya_ai_player_create at svc_ai_player.c:629
[01-01 00:00:01 ty E][ai_audio_player.c:204] ret:-3
[01-01 00:00:01 ty E][ai_chat_main.c:417] ret:-3
[01-01 00:00:01 ty E][app_chat_bot.c:170] ret:-3
[01-01 00:00:01 ty E][tuya_main.c:379] app_chat_bot_init failed: -3
[01-01 00:00:01 ty I][tuya_main.c:389] [Phase-4 done] Heap after app init: 12820
```

关键数字：尝试分配 `0x2000 = 8192 B`，报告空闲 `0x21dc = 8668 B`，但因堆碎片化而失败。

同时还出现线程创建失败：
```
[01-01 00:00:01 ty E][tal_thread.c:215] Create Thrd Fail:-26624
```

## 根因

### 内存消耗路径

Phase-3 完成后堆约 **85KB** 可用，Phase-4 中 AI 管道按以下顺序分配：

| 分配项 | 默认大小 | 说明 |
|--------|----------|------|
| `AI_OUTPUT_RINGBUF_SIZE` | **65,536 B（64KB）** | WebSocket 流出 ring buffer |
| `AI_INPUT_RINGBUF_SIZE` | **65,536 B（64KB）** | WebSocket 流入 ring buffer |
| `AI_PLAYER_RINGBUF_SIZE` | 16,384 B | 解码后 PCM ring buffer |
| `AI_PLAYER_DECODEBUF_SIZE` | 8,192 B ← **分配失败** | 解码器工作缓冲区 |
| 各任务栈、结构体等 | ~10KB | 多线程开销 |

AI_OUTPUT_RINGBUF（64KB）首先成功分配后，剩余堆严重碎片化，后续 8192B 分配因找不到连续块而失败。

### 为什么配置改了但没生效

虽然在 `app_default.config` 中写入了较小的 buffer 值，但 **cmake 缓存（`.build` 目录）未清除**。TuyaOpen 的外层 cmake 只在首次配置时读取 `app_default.config` 并生成 `build_param.cmake`；之后的增量构建复用旧缓存，导致配置更改被忽略。

验证方法：查看 `apps/tuya.ai/petoi_esp32c3_cube/.build/build/build_param.cmake` 中的实际值。

## 修复

### 1. 补全 `apps/tuya.ai/petoi_esp32c3_cube/app_default.config`

加入所有 AI buffer 缩减条目（覆盖 Kconfig 默认值）：

```ini
# --- ESP32-C3 memory budget (no PSRAM, ~85 KB free after net/BLE init) ---
CONFIG_AI_INPUT_RINGBUF_SIZE=8192
CONFIG_AI_OUTPUT_RINGBUF_SIZE=8192
CONFIG_AI_PLAYER_RINGBUF_SIZE=4096
CONFIG_AI_PLAYER_DECODEBUF_SIZE=2048
CONFIG_AI_PLAYER_FRAMEBUF_SIZE=2048
CONFIG_AI_PLAYER_STACK_SIZE=3072
CONFIG_AI_WRITE_SOCKET_BUF_SIZE=4096
```

### 2. 清除 cmake 缓存并重新编译

```bash
cd apps/tuya.ai/petoi_esp32c3_cube
rm -rf .build
python3 /path/to/TuyaOpen/tos.py build
```

> **重要**：每次修改 `app_default.config` 后，必须删除 `.build` 目录，否则更改不会生效。

### 3. 验证生效

```bash
grep "AI_.*RINGBUF\|AI_.*BUF_SIZE\|AI_WRITE" \
  apps/tuya.ai/petoi_esp32c3_cube/.build/build/build_param.cmake
```

应输出：
```
set(CONFIG_AI_INPUT_RINGBUF_SIZE "8192")
set(CONFIG_AI_OUTPUT_RINGBUF_SIZE "8192")
set(CONFIG_AI_PLAYER_RINGBUF_SIZE "4096")
set(CONFIG_AI_PLAYER_DECODEBUF_SIZE "2048")
set(CONFIG_AI_PLAYER_FRAMEBUF_SIZE "2048")
set(CONFIG_AI_PLAYER_STACK_SIZE "3072")
set(CONFIG_AI_WRITE_SOCKET_BUF_SIZE "4096")
```

## 内存优化效果

| Buffer | 旧默认值 | 新值 | 节省 |
|--------|----------|------|------|
| AI_INPUT_RINGBUF | 64 KB | 8 KB | 56 KB |
| AI_OUTPUT_RINGBUF | 64 KB | 8 KB | 56 KB |
| AI_PLAYER_RINGBUF | 16 KB | 4 KB | 12 KB |
| AI_PLAYER_DECODEBUF | 8 KB | 2 KB | 6 KB |
| AI_PLAYER_FRAMEBUF | 4 KB | 2 KB | 2 KB |
| AI_WRITE_SOCKET | 8 KB | 4 KB | 4 KB |
| **合计节省** | **164 KB** | **28 KB** | **~136 KB** |

## 注意事项

- `AI_PLAYER_DECODEBUF_SIZE=2048` 是 Opus 解码最小可用值；若出现解码失败可适当增大
- `AI_INPUT_RINGBUF_SIZE=8192` 和 `AI_OUTPUT_RINGBUF_SIZE=8192` 在低带宽场景下可能出现缓冲区不足，如遇卡顿可适当增大（需权衡内存）

## 关联：`lfs open` / `malloc failed:0x1000`（LittleFS 文件缓存）

在无 PSRAM、且 MP3 `ctx`/LVGL 等已占用大量内部 RAM 时，可能出现：

- `[Phase-4 done] Heap` 约 **4 KB+**，但紧接着 **`lfs_file_rawopencfg` → `malloc(0x1000)` 失败**
- 日志里 **`free` 略大于 4096**（例如 `0x10bc`）仍失败 → **堆碎片化**，没有连续 **4 KB** 块

原因：`tal_kv_init` 曾将 **`lfs_cfg.cache_size == block_size`（常为 4096）**，每次 `lfs_file_open` 会为文件缓存 **`malloc(cache_size)`**。

**修复**（`src/tal_kv/src/tal_kv.c`）：当 `block_size >= 4096` 且为 1024 的倍数时，使用 **`read_size/prog_size = 256`**、**`cache_size = 1024`**（仍为 `block_size` 的约数，**不改变盘上 superblock 的 block_size**）。这样单次打开文件约 **1 KB** 堆，且 **`lfs_mount` 时 rcache/pcache 从 `2×4KB` 降为 `2×1KB`**，Phase-1 起就多出约 **6 KB** 可用堆。

## 关联：`ap_netcfg_init` / `malloc failed:0x10e4`

`ap_netcfg_t` 内含 **`recv_buf[4096]`**，整体约 **4.3 KiB**。绑定时再 `tal_malloc(sizeof(ap_netcfg_t))` 易因碎片失败，日志表现为 **`netcfg type 0x1 is not regist`**（AP 配网未挂上），手机端若优先走 AP 会报错；**BLE 配网（0x2）仍可成功**。

**修复**（`src/tuya_cloud_service/netcfg/ap_netcfg.c`）：使用 **BSS 静态单例** `s_ap_netcfg_storage`，不再为 AP 模块申请整块堆内存。

在无 PSRAM 的 **ESP32-C3** 上，即使 AP 模块已不 `malloc`，**启动 SoftAP** 仍可能因 WiFi 内部分配失败而 **在 `ieee80211_hostap_attach` 崩溃**（见 `docs/issue_runtime_esp32c3_softap_ble_coexist_crash.md`）。**PETOI** 工程改为 **仅 BLE 配网**，不再启用 `NETCFG_TUYA_WIFI_AP`。

## 关联：PETOI 屏亮无图 / 全黑

ST7789 公共驱动 **`lcd_st7789_spi.c`** 曾把 **`DISPLAY_BACKLIGHT_OUTPUT_INVERT`（背光极性）误传给 `esp_lcd_panel_invert_color`**。PETOI 在 **`board_display_init`** 中补 **`tkl_gpio` 打开背光**（`DISPLAY_BACKLIGHT_PIN`），并在 **`board_config.h`** 定义 **`DISPLAY_ST7789_COLOR_INVERT`** 与背光极性解耦。
