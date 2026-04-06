# your_chat_bot 移植 ESP32-C3 内存可行性评估

> 日期：2026-04-06
> 范围：`apps/tuya.ai/your_chat_bot` 移植到 ESP32-C3（无 PSRAM）的内存使用分析与裁剪建议

---

## 一、背景：PSRAM 相关宏定义

### 1.1 控制外置 PSRAM 的核心宏：`ENABLE_EXT_RAM`

**定义位置**：`tools/porting/template/Kconfig`，第 127 行：

```kconfig
config ENABLE_EXT_RAM
    bool "ENABLE_EXT_RAM --- support external ram ex: PSRAM"
    default n
```

默认值为 `n`（禁用）。该宏开启后，以下内存分配行为会切换到 PSRAM：

| 影响模块 | 具体行为 |
|----------|----------|
| `src/tal_system/include/tal_memory.h` | `Malloc()`/`Calloc()`/`Free()` 三个全局宏重定向至 `tal_psram_malloc()`/`tal_psram_calloc()`/`tal_psram_free()` |
| `apps/tuya.ai/your_chat_bot/src/tuya_main.c` | cJSON 内存钩子切换为 PSRAM 分配器 |
| `apps/tuya.ai/your_chat_bot/src/display2/app_display.c` | 显示 Frame Buffer 改用 `tal_psram_malloc` 分配 |
| `apps/tuya.ai/your_chat_bot/src/app_chat_bot.c` | 运行时打印 PSRAM 空闲堆大小 |
| `src/audio_player/` 音频解码器 | MP3/Opus/OggOpus 解码缓冲区改用 PSRAM |
| `src/tuya_cloud_service/tls/tuya_tls.c` | TLS 握手缓冲区改用 PSRAM |
| `src/tuya_ai_service/` AI 服务各模块 | AI 输入/输出/编码器缓冲区改用 PSRAM |
| `apps/tuya.ai/ai_components/ai_audio/src/ai_audio_input.c` | 录音 ring buffer 改用 PSRAM |

**当前状态**：`your_chat_bot` 的所有 ESP32-S3 配置文件（`DNESP32S3_BOX.config`、`DNESP32S3.config`、`app_default.config`）均未设置 `CONFIG_ENABLE_EXT_RAM=y`，即当前代码**不使用 PSRAM**。

### 1.2 辅助宏：`DISPLAY_BUFF_SPIRAM` / `DISPLAY_BUFF_DMA`

**定义位置**：各 board 的 `board_config.h`，例如 `boards/ESP32/DNESP32S3_BOX/board_config.h`：

```c
// Only one of DISPLAY_BUFF_SPIRAM and DISPLAY_BUFF_DMA can be selected
#define DISPLAY_BUFF_SPIRAM 0
#define DISPLAY_BUFF_DMA    1
```

这两个宏**仅控制 LVGL 渲染缓冲区（Render Buffer）的分配位置**，与系统整体 PSRAM 使用无关：

| 宏 | 内存位置 | 速度 | 特点 |
|---|---------|------|------|
| `DISPLAY_BUFF_DMA = 1`（当前） | 芯片内部 SRAM（DMA-capable 区域） | 快，低延迟 | LCD 刷屏原生 DMA 支持，适合高刷新率 |
| `DISPLAY_BUFF_SPIRAM = 1` | 芯片外部 PSRAM | 慢，通过 SPI 总线 | 内部 RAM 不足时的备用方案 |

> **结论**：当前 `DISPLAY_BUFF_DMA=1`，LVGL 绘图缓冲区不使用 PSRAM，完全在内部 RAM 中分配。

---

## 二、ESP32-C3 实际可用内存

"将内存优化到 400KB 以内"需要先澄清概念：400KB 是 ESP32-C3 的**芯片全部内部 SRAM 总量**，并非可用堆空间。

```
ESP32-C3 总 SRAM：~400 KB
  ├── 代码/IRAM：        约 150~180 KB（WiFi 驱动、协议栈代码段）
  ├── WiFi 运行时堆：     约  90~110 KB
  └── IDF 系统任务栈：   约  20~ 30 KB
  ─────────────────────────────────────
  应用可用堆（实测）：    约   85 KB
```

> 数据来源：`apps/tuya.ai/petoi_esp32c3_cube/app_default.config` 第 9 行注释：  
> `# ESP32-C3 memory budget (no PSRAM, ~85 KB free after net/BLE init)`

**真正的目标是：应用层所有缓冲区 + 线程栈 ≤ 约 85 KB。**

---

## 三、当前 your_chat_bot 默认配置的内存开销

### 3.1 线程栈

| 任务 | 栈大小 | 来源文件 |
|------|--------|---------|
| 应用主线程 | 4096 B | `apps/tuya.ai/your_chat_bot/src/tuya_main.c` |
| ai_chat_main 模式任务 | 3072 B | `apps/tuya.ai/ai_components/ai_main/src/ai_chat_main.c` |
| 录音 record_task | 2560 B | `apps/tuya.ai/ai_components/ai_audio/src/ai_audio_input.c` |
| AI Input 任务 | 4608 B | `src/tuya_ai_service/svc_ai_agent/Kconfig` 默认值 |
| AI Output 任务 | 4096 B | `src/tuya_ai_service/svc_ai_agent/Kconfig` 默认值 |
| AI Client 任务 | 4096 B | `src/tuya_ai_service/svc_ai_basic/Kconfig` 默认值 |
| AI Biz 任务 | 4096 B | `src/tuya_ai_service/svc_ai_basic/src/tuya_ai_biz.c` |
| AI Player 任务（若开） | 5120 B | `src/audio_player/Kconfig` 默认值 |
| **线程栈小计** | **约 31~36 KB** | |

### 3.2 动态缓冲区（Kconfig 默认值）

| 缓冲区 | 默认大小 | Kconfig 配置项 | 来源文件 |
|--------|---------|---------------|---------|
| AI Input Ring | **65536 B（64 KB）** | `AI_INPUT_RINGBUF_SIZE` | `svc_ai_agent/Kconfig` |
| AI Output Ring | **65536 B（64 KB）** | `AI_OUTPUT_RINGBUF_SIZE` | `svc_ai_agent/Kconfig` |
| AI Input Buf | 6144 B | `AI_INPUT_BUF_SIZE` | `svc_ai_agent/Kconfig` |
| AI Output Buf | 5120 B | `AI_OUTPUT_BUF_SIZE` | `svc_ai_agent/Kconfig` |
| AI Write Socket Buf | 8192 B | `AI_WRITE_SOCKET_BUF_SIZE` | `svc_ai_basic/Kconfig` |
| AI 协议 Fragment 接收 | 10240 B × 2 session | `AI_MAX_FRAGMENT_LENGTH` × `AI_SESSION_MAX_NUM` | `svc_ai_basic/Kconfig` |
| AI Player Ring（若开） | 16384 B | `AI_PLAYER_RINGBUF_SIZE` | `audio_player/Kconfig` |
| AI Player DecodeBuf（若开） | 8192 B | `AI_PLAYER_DECODEBUF_SIZE` | `audio_player/Kconfig` |
| AI Player FrameBuf（若开） | 4096 B | `AI_PLAYER_FRAMEBUF_SIZE` | `audio_player/Kconfig` |
| 音频输入 Ring（VAD） | ~15600 B | — | `ai_audio_input.c`（16 kHz/单声道，VAD=200ms+300ms） |
| TLS I/O 明文缓冲 | ~8192 B（4 KB × 2） | `ENABLE_MBEDTLS_SSL_MAX_CONTENT_LEN` | `src/libtls/port/tuya_tls_config.h` |
| **缓冲区小计（含 Player）** | **约 222 KB** | | |

### 3.3 汇总对比

```
默认配置总开销：  ~31 KB（栈）+ ~222 KB（缓冲） ≈ 253 KB
ESP32-C3 可用堆：~85 KB

超出约 168 KB ← 直接移植完全不可行
```

> 仅 AI Input Ring（64 KB）+ AI Output Ring（64 KB）两项就已超过 C3 全部可用堆。

---

## 四、petoi_esp32c3_cube 已有的裁剪方案

`apps/tuya.ai/petoi_esp32c3_cube/app_default.config` 第 9~27 行提供了完整注释和具体裁剪数值，可作为移植 your_chat_bot 的直接参考：

```ini
# --- ESP32-C3 memory budget (no PSRAM, ~85 KB free after net/BLE init) ---
#
# Default values and their impact on heap:
#   AI_INPUT_RINGBUF   64 KB  → 8 KB   (streaming input from mic to AI agent)
#   AI_OUTPUT_RINGBUF  64 KB  → 8 KB   (streaming output from AI agent)
#   AI_PLAYER_RINGBUF  16 KB  → 4 KB   (decoded PCM ring buffer)
#   AI_PLAYER_DECODEBUF 8 KB  → 2 KB   (decoder working buffer)
#   AI_PLAYER_FRAMEBUF  4 KB  → 2 KB   (single decode frame)
#   AI_PLAYER_STACK     5 KB  → 3 KB
#   AI_WRITE_SOCKET     8 KB  → 4 KB   (WebSocket send buffer)
#
# Total saved: ~147 KB → ~31 KB  (frees ~116 KB for other subsystems)
CONFIG_AI_INPUT_RINGBUF_SIZE=8192
CONFIG_AI_OUTPUT_RINGBUF_SIZE=8192
CONFIG_AI_PLAYER_RINGBUF_SIZE=4096
CONFIG_AI_PLAYER_DECODEBUF_SIZE=2048
CONFIG_AI_PLAYER_FRAMEBUF_SIZE=2048
CONFIG_AI_PLAYER_STACK_SIZE=3072
CONFIG_AI_WRITE_SOCKET_BUF_SIZE=4096
```

各项节省效果：

| 配置项 | 默认值 | C3 裁剪值 | 节省 |
|--------|--------|-----------|------|
| `AI_INPUT_RINGBUF_SIZE` | 64 KB | **8 KB** | 56 KB |
| `AI_OUTPUT_RINGBUF_SIZE` | 64 KB | **8 KB** | 56 KB |
| `AI_PLAYER_RINGBUF_SIZE` | 16 KB | **4 KB** | 12 KB |
| `AI_PLAYER_DECODEBUF_SIZE` | 8 KB | **2 KB** | 6 KB |
| `AI_PLAYER_FRAMEBUF_SIZE` | 4 KB | **2 KB** | 2 KB |
| `AI_PLAYER_STACK_SIZE` | 5 KB | **3 KB** | 2 KB |
| `AI_WRITE_SOCKET_BUF_SIZE` | 8 KB | **4 KB** | 4 KB |
| **合计节省** | | | **≈ 138 KB** |

裁剪后缓冲区合计约 **31~33 KB**，加线程栈约 **28 KB** = **总约 59~61 KB**，勉强在 85 KB 之内。

---

## 五、your_chat_bot 移植还需要的额外裁剪

以下是 petoi 未涉及、但 your_chat_bot 移植时需要额外注意的差异项：

| 项目 | your_chat_bot 现状 | 建议裁剪 | 节省 |
|------|-------------------|---------|------|
| 日志缓冲 | `tal_log_init(1024)` | 改为 `512` | 0.5 KB |
| 应用主线程栈 | 4096 B | 改为 3072 B | 1 KB |
| `AI_SESSION_MAX_NUM` | 默认 2（Fragment 缓冲 × 2，共 20 KB） | 改为 1 | 10 KB |
| 显示 / LVGL | `app_default.config` 已关闭 ✅ | 保持关闭 | — |
| TTS Player | `app_default.config` 已关闭 ✅ | 保持关闭；若开启必须同步限制 ring 大小 | — |
| `ENABLE_AI_MONITOR` | 默认开启 | 建议关闭，节省监控任务资源 | ~2 KB |
| cJSON hooks | C3 无 PSRAM，自动走内部堆 ✅ | 无需改动 | — |
| `ENABLE_EXT_RAM` | 当前未启用 ✅ | 保持禁用（C3 无 PSRAM） | — |

---

## 六、总结与结论

| 评估项 | 结论 |
|--------|------|
| 直接移植，不改配置 | **不可行**。仅默认 AI Ring 就超出 C3 可用堆约 168 KB |
| 按 petoi 方案裁剪配置 | **理论可行**，优化后约 59~61 KB，保留约 24 KB 余量，但余量极小 |
| 是否需要修改 C 源代码 | **基本不需要**。petoi 的全部裁剪均通过 `.config` 文件中 Kconfig 参数完成 |
| 主要风险 | 余量紧张，若新增带屏、TTS、MCP 工具等功能会很快耗尽堆 |
| 推荐路径 | 以 `petoi_esp32c3_cube/app_default.config` 为模板，逐步叠加 `your_chat_bot` 的功能，每步监控 `heap_caps_get_free_size()` |

---

## 附录：Kconfig 配置项速查

| 配置项 | 默认值 | C3 建议值 | 定义位置 |
|--------|--------|-----------|---------|
| `AI_INPUT_RINGBUF_SIZE` | 65536 | 8192 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_OUTPUT_RINGBUF_SIZE` | 65536 | 8192 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_INPUT_BUF_SIZE` | 6144 | 4096 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_OUTPUT_BUF_SIZE` | 5120 | 4096 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_INPUT_STACK_SIZE` | 4608 | 3072 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_OUTPUT_STACK_SIZE` | 4096 | 3072 | `src/tuya_ai_service/svc_ai_agent/Kconfig` |
| `AI_CLIENT_STACK_SIZE` | 4096 | 3072 | `src/tuya_ai_service/svc_ai_basic/Kconfig` |
| `AI_MAX_FRAGMENT_LENGTH` | 10240 | 4096 | `src/tuya_ai_service/svc_ai_basic/Kconfig` |
| `AI_SESSION_MAX_NUM` | 2 | 1 | `src/tuya_ai_service/svc_ai_basic/Kconfig` |
| `AI_WRITE_SOCKET_BUF_SIZE` | 8192 | 4096 | `src/tuya_ai_service/svc_ai_basic/Kconfig` |
| `AI_PLAYER_RINGBUF_SIZE` | 16384 | 4096 | `src/audio_player/Kconfig` |
| `AI_PLAYER_DECODEBUF_SIZE` | 8192 | 2048 | `src/audio_player/Kconfig` |
| `AI_PLAYER_FRAMEBUF_SIZE` | 4096 | 2048 | `src/audio_player/Kconfig` |
| `AI_PLAYER_STACK_SIZE` | 5120 | 3072 | `src/audio_player/Kconfig` |
| `ENABLE_AI_MONITOR` | y | n | `src/tuya_ai_service/svc_ai_basic/Kconfig` |

## 附录：关键文件索引

| 文件 | 说明 |
|------|------|
| `apps/tuya.ai/your_chat_bot/app_default.config` | your_chat_bot 默认配置（当前 S3 配置，未裁剪） |
| `apps/tuya.ai/your_chat_bot/config/DNESP32S3_BOX.config` | DNESP32S3_BOX 板配置 |
| `apps/tuya.ai/petoi_esp32c3_cube/app_default.config` | C3 内存预算参考配置（含详细注释） |
| `apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c` | C3 分阶段启动与日志裁剪参考 |
| `apps/tuya.ai/ai_components/ai_audio/src/ai_audio_input.c` | 音频录入 ring buffer 大小计算逻辑 |
| `src/tuya_ai_service/svc_ai_agent/Kconfig` | AI Input/Output 任务栈与 ring 大小配置 |
| `src/tuya_ai_service/svc_ai_basic/Kconfig` | AI Client、Fragment、Session 数等配置 |
| `src/audio_player/Kconfig` | AI Player 各缓冲区大小配置 |
| `src/tal_system/include/tal_memory.h` | `ENABLE_EXT_RAM` 宏控制 `Malloc`/`Free` 路由 |
| `tools/porting/template/Kconfig` | `ENABLE_EXT_RAM` 宏的 Kconfig 定义 |
| `boards/ESP32/DNESP32S3_BOX/board_config.h` | S3 板级 `DISPLAY_BUFF_SPIRAM`/`DMA` 定义 |
| `boards/ESP32/common/display/lv_port_disp.c` | LVGL 显示缓冲 flag 的实际使用位置 |
| `src/libtls/port/tuya_tls_config.h` | `MBEDTLS_SSL_MAX_CONTENT_LEN` 配置 |
