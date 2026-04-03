# 修复：ESP32-C3 启动时堆损坏崩溃（音频 AFE 内存耗尽）

## 崩溃现象

ESP32-C3 烧录后程序启动即崩溃，不断重启。关键日志：

```
W (1578) AFE_CONFIG: wakenet model not found. please load wakenet model...
W (1588) AFE_CONFIG: Noise Supression may reduce the accuracy of speech recognition.
E (1598) SR_RINGBUF: ./components/dl_lib/sr_ringbuf.c:95 (sr_rb_create): Memory exhausted
CORRUPT HEAP: multi_heap.c:116 detected at 0x3fca3294
abort() was called at PC 0x40390393 on core 0
```

## 崩溃调用栈分析

```
user_main()
  └→ app_chat_bot_init()
      └→ ai_chat_init()
          └→ ai_audio_input_init()
              └→ tdl_audio_open()
                  └→ __tdd_audio_esp_i2s_8311_open()    [tdd_audio_8311_codec.c:367]
                      └→ audio_afe_processor_init()
                          └→ __esp_afe_init()            [audio_afe.c:157]
                              └→ afe_create_from_config()
                                  └→ sr_rb_create()      → Memory exhausted
                                      └→ sr_rb_destroy()
                                          └→ free()      → CORRUPT HEAP → abort()
```

## 根因

| 因素 | 当前配置 | ESP32-C3 实际情况 |
|------|---------|------------------|
| AFE 工作模式 | `AFE_MODE_HIGH_PERF` | 需要大量内存，远超可用堆 |
| 内存分配策略 | `AFE_MEMORY_ALLOC_MORE_PSRAM` | ESP32-C3 **无 PSRAM** |
| CPU 核心指定 | `afe_perferred_core = 1` | ESP32-C3 **单核**（仅 Core 0） |
| 可用堆内存 | AFE 需要 >200 KB | Phase-3 后仅剩 ~91 KB |

### 内存时间线

| 阶段 | 可用堆 (bytes) | 说明 |
|------|---------------|------|
| Phase-1 (bootstrap) | 186,548 | KV/timer/CLI 初始化后 |
| Phase-2 (cloud/net) | 91,900 | WiFi + BLE + MQTT 初始化后 |
| Phase-3 (hardware) | 91,404 | 板级硬件注册后 |
| AFE 初始化 | 需要 >200 KB | **远超可用内存 → OOM → 堆损坏** |

### 堆损坏机制

1. `afe_create_from_config()` 进行多次内部 `malloc`
2. `sr_rb_create()` 分配失败（Memory exhausted）
3. ESP-SR 库尝试清理已分配的内存，但部分清理路径写入了无效地址
4. `free()` 在堆元数据损坏的块上触发 `assert_valid_block` → `abort()`

## 修复方案

### 1. 创建平台层 `audio_afe.c`（核心修复）

**路径：** `platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/audio/audio_afe.c`

使用 `#ifdef CONFIG_SPIRAM` 编译期门控：

- **有 PSRAM：** 正常初始化 ESP-SR AFE（使用 `AFE_MODE_LOW_COST` + `core 0`）
- **无 PSRAM：** 跳过 AFE 初始化，打印警告，返回 `OPRT_OK`
- `auio_afe_processor_feed()` 在 AFE 未初始化时静默丢弃音频帧

### 2. 创建 `audio_afe.h` 头文件

**路径：** `platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/audio/audio_afe.h`

提供 `audio_afe_processor_init()` 和 `auio_afe_processor_feed()` 声明。

### 3. 所有 codec 驱动统一处理 AFE 失败为非致命

**涉及文件：**
- `boards/ESP32/common/audio/tdd_audio_no_codec.c`
- `boards/ESP32/common/audio/tdd_audio_atk_no_codec.c`
- `boards/ESP32/common/audio/tdd_audio_es8388_codec.c`
- `boards/ESP32/common/audio/tdd_audio_es8389_codec.c`

将 `audio_afe_processor_init()` 失败从 `PR_ERR + return error` 改为 `PR_WARN + continue`，
确保音频编解码器在无 AFE 时仍能正常工作（push-to-talk 模式）。

`tdd_audio_8311_codec.c` 已在之前的提交中修复。

## 修复后预期行为

```
W (xxxx) audio_afe: No PSRAM – AFE processor skipped (use manual VAD / push-to-talk mode)
[xx-xx xx:xx:xx ty D][tdd_audio_8311_codec.c:379] I2S 8311 read task args: 0x3fccxxxx
```

- 程序正常启动，不再崩溃
- ESP32-C3 以 push-to-talk（手动 VAD，mode 0）模式运行
- 唤醒词检测和自动 VAD 不可用（需要 PSRAM）
- 音频录放正常工作

## 注意事项

- `auio_afe_processor_feed()` 函数名中的 typo（`auio` 而非 `audio`）保持不变，
  以兼容现有 5 个 codec 驱动的调用
- 无 PSRAM 平台必须在应用层配置 `vad_mode = AI_AUDIO_VAD_MANUAL`（mode 0）
- `app_default.config` 中的 `CONFIG_AI_*` 缓冲大小已在之前的提交中缩减
