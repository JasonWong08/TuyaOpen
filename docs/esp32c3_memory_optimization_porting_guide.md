# ESP32-C3 内存优化移植指南

> 从 ESP32-S3 (your_chat_bot) 移植到 ESP32-C3 的内存优化方案。
> 参考项目：`petoi_esp32c3_cube`。

## 硬件对比

| 参数 | ESP32-S3 (当前) | ESP32-C3 (目标) | 差距 |
|------|-----------------|-----------------|------|
| 内部 SRAM | 512 KB (~334 KB 可用) | 400 KB (~280 KB 可用) | -54 KB |
| PSRAM | 8 MB | **无** | **-8 MB** |
| Flash | 16 MB | 16 MB | 相同 |
| CPU | 双核 Xtensa 240 MHz | 单核 RISC-V 160 MHz | 性能降低 |

**核心挑战：** 最大问题不是 SRAM 缩了 54 KB，而是**失去了 8 MB PSRAM**。当前 S3 上有约 232 KB 的大块缓冲通过 ESP-IDF 的 `CONFIG_SPIRAM_USE_MALLOC` 透明分配到 PSRAM；C3 上这些必须全部挤进内部 SRAM。

---

## 第一步：缩减缓冲区（配置层，不改代码，节省 ~140 KB）

参考 `petoi_esp32c3_cube` 的 `app_default.config`，在 `your_chat_bot` 的 C3 配置文件中覆盖以下 Kconfig 项：

| 配置项 | S3 默认值 | C3 推荐值 | 节省 | 影响说明 |
|--------|----------|----------|------|---------|
| `AI_INPUT_RINGBUF_SIZE` | 64 KB | 8 KB | 56 KB | 麦克风到 AI 的上行缓冲，8 KB 约 250 ms 音频，需保证网络不卡顿 |
| `AI_OUTPUT_RINGBUF_SIZE` | 64 KB | 8 KB | 56 KB | AI 到播放器的下行缓冲，同上 |
| `AI_PLAYER_RINGBUF_SIZE` | 16 KB | 4 KB | 12 KB | 解码后 PCM 环形缓冲 |
| `AI_PLAYER_DECODEBUF_SIZE` | 8 KB | 2 KB | 6 KB | 解码器工作缓冲 |
| `AI_PLAYER_FRAMEBUF_SIZE` | 4 KB | 2 KB | 2 KB | 单帧读取缓冲 |
| `AI_PLAYER_STACK_SIZE` | 5 KB | 3 KB | 2 KB | 播放线程栈 |
| `AI_WRITE_SOCKET_BUF_SIZE` | 8 KB | 4 KB | 4 KB | WebSocket 发送缓冲 |
| Log buffer (代码中) | 1024 | 512 | 0.5 KB | 日志缓冲 |
| App thread stack (代码中) | 4096 | 3072 | 1 KB | 应用主线程栈 |
| **合计** | | | **~140 KB** | |

### 配置文件示例

文件路径：`config/YOUR_ESP32C3_BOARD.config`

```ini
CONFIG_PROJECT_VERSION="1.0.1"
CONFIG_TUYA_PRODUCT_ID="your_product_id"
# CONFIG_ENABLE_COMP_AI_DISPLAY is not set
# CONFIG_ENABLE_AI_AUDIO_TTS_PLAY is not set
CONFIG_BOARD_CHOICE_ESP32=y
CONFIG_BOARD_CHOICE_YOUR_ESP32C3_BOARD=y
CONFIG_ENABLE_BUTTON=y
CONFIG_BUTTON_NAME="ai_chat_button"
CONFIG_AI_INPUT_RINGBUF_SIZE=8192
CONFIG_AI_OUTPUT_RINGBUF_SIZE=8192
CONFIG_AI_PLAYER_RINGBUF_SIZE=4096
CONFIG_AI_PLAYER_DECODEBUF_SIZE=2048
CONFIG_AI_PLAYER_FRAMEBUF_SIZE=2048
CONFIG_AI_PLAYER_STACK_SIZE=3072
CONFIG_AI_WRITE_SOCKET_BUF_SIZE=4096
```

---

## 第二步：关闭不需要的功能模块（配置层，节省 ~30-50 KB）

在已关闭屏幕显示和 TTS 播放的基础上，C3 移植还可以考虑：

| 功能 | 配置 | 预估节省 | 是否影响核心对话 |
|------|------|---------|----------------|
| 屏幕显示 | `# CONFIG_ENABLE_COMP_AI_DISPLAY is not set` | ~4.5 KB 静态 + ~26 KB 运行时 | 否（已关闭） |
| TTS 播放 | `# CONFIG_ENABLE_AI_AUDIO_TTS_PLAY is not set` | 功能级关闭 | 否（已关闭） |
| AI Monitor | `# CONFIG_ENABLE_AI_MONITOR is not set` | ~2-4 KB | 否，调试工具 |
| MCP Server | `# CONFIG_ENABLE_COMP_AI_MCP is not set` | ~3-5 KB | 否，扩展功能 |
| 多余对话模式 | 只保留 `CONFIG_ENABLE_COMP_AI_MODE_HOLD=y`，关闭 oneshot/wakeup/free | ~6-10 KB (代码+栈) | 视需求 |

---

## 第三步：代码层修改（必须改的）

### 3.1 `tuya_main.c` 中的日志和线程栈

```c
// 日志缓冲从 1024 减到 512
tal_log_init(TAL_LOG_LEVEL_DEBUG, 512, (TAL_LOG_OUTPUT_CB)tkl_log_output);

// 应用线程栈从 4096 减到 3072
thrd_param.stackDepth = 3072;
```

### 3.2 修复 `ai_audio_input.c` 中的 PSRAM 硬编码

`ai_components/ai_audio/src/ai_audio_input.c` 中创建录音 ringbuf 时始终使用 `OVERFLOW_PSRAM_STOP_TYPE`，未按 `ENABLE_EXT_RAM` 分支。C3 无 PSRAM 时需确认该 overflow 类型是否回退到内部 SRAM。如果不行，需修改为：

```c
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    TUYA_CALL_ERR_GOTO(tuya_ring_buff_create(rb_size, OVERFLOW_PSRAM_STOP_TYPE, &sg_recorder->ringbuf), __error);
#else
    TUYA_CALL_ERR_GOTO(tuya_ring_buff_create(rb_size, OVERFLOW_STOP_TYPE, &sg_recorder->ringbuf), __error);
#endif
```

### 3.3 cJSON 分配器

C3 无 PSRAM，确保走内部堆：

```c
cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
```

> `your_chat_bot` 当前 `ENABLE_EXT_RAM` 未定义时已走此路径，无需额外修改。

---

## 第四步：创建板级定义

目录：`boards/ESP32/YOUR_ESP32C3_BOARD/`

参考 `PETOI_ESP32C3_CUBE`，创建板级文件：

| 文件 | 说明 |
|------|------|
| `Kconfig` | 设置 `CHIP_CHOICE="esp32c3"`，select 所需外设 |
| `board_config.h` | 按硬件定义 I2S/I2C/GPIO 引脚、DMA 参数 |
| `board_com_api.h` / `.c` | 板级初始化 |

### 关键优化点（参考 Petoi 的 `board_config.h`）

- `AUDIO_CODEC_DMA_DESC_NUM` 从 6 减到 3
- 如有显示，LVGL 缓冲用 DMA SRAM（`DISPLAY_BUFF_DMA 1`）

---

## 第五步：内存预算验证

移植后预估的 C3 内存布局：

| 内存区域 | 估算占用 | 可用总量 | 剩余 |
|---------|---------|---------|------|
| 静态 SRAM (.text+.data+.bss) | ~137 KB | ~280 KB | ~143 KB |
| 系统服务线程栈 | ~20 KB | | |
| AI Agent (缩减后) | ~31 KB | | |
| 音频子系统 (缩减后) | ~30 KB | | |
| 对话模式 + IoT/WiFi/BLE | ~40-60 KB | | |
| **运行时合计** | **~121-141 KB** | | |
| **总计** | **~258-278 KB** | **~280 KB** | **~2-22 KB** |

### 结论

**内存预算非常紧张。** 建议按以下优先级实施：

1. **优先实现配置层优化**（第一、二步），构建后用 `idf_size.py` 检查静态占用。
2. **实际烧录后观察**串口 `Heap: free=xxx` 日志确认运行时余量。
3. **如果余量不足**，进一步缩减：
   - `AI_INPUT_BUF_SIZE`：6144 → 4096
   - `AI_OUTPUT_BUF_SIZE`：5120 → 4096
   - `AI_MAX_FRAGMENT_LENGTH`：10240 → 8192
4. **最终手段**——缩减系统栈：
   - `STACK_SIZE_WORK_QUEUE`：5120 → 4096
   - `TUYA_HS_BLE_HOST_STACK_SIZE`：5120 → 4096
