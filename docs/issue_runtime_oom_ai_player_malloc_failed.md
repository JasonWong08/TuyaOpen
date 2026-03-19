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
