# 问题：运行时崩溃（AFE 内存耗尽 + 堆损坏）

## 现象

程序启动后在 ES8311 音频编解码器初始化完成后立即崩溃：

```
W (2191) AFE_CONFIG: wakenet model not found. please load wakenet model...
W (2201) AFE_CONFIG: Noise Supression may reduce the accuracy of speech recognition. ...
E (2211) SR_RINGBUF: ./components/dl_lib/sr_ringbuf.c:95 (sr_rb_create): Memory exhausted
CORRUPT HEAP: multi_heap.c:116 detected at 0x3fcc0014
abort() was called at PC 0x40392419 on core 0
```

崩溃点在 `sr_rb_create`（ESP-SR 库内部的 ring buffer 分配），随后发生堆损坏并 abort。

## 根因

### 调用链

```
tdd_audio_8311_codec.c:367
  → audio_afe_processor_init()
    → __esp_afe_init()
      afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF)
      afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM  ← 问题！
      afe_config->afe_perferred_core = 1                           ← 问题！
      afe_iface->create_from_config(afe_config)
        → sr_rb_create()  → Memory exhausted → 堆损坏 → abort
```

### 具体原因

| 配置项 | 问题值 | ESP32-C3 实际情况 |
|--------|--------|------------------|
| `AFE_MODE_HIGH_PERF` | 高性能模式 | 内部 RAM 仅 ~85KB 可用 |
| `AFE_MEMORY_ALLOC_MORE_PSRAM` | 优先 PSRAM 分配 | ESP32-C3 **无 PSRAM** |
| `afe_perferred_core = 1` | 指定 Core 1 | ESP32-C3 **单核**，Core 1 不存在 |

Phase-3 后堆剩余约 85KB，AFE 高性能模式需要远超此数的连续内存（PSRAM 通常有 4MB+），`sr_rb_create` 无法分配而失败。失败时 ESP-SR 库已做部分分配，清理不完整导致堆损坏，最终 abort。

## 修复

### 1. `platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/audio/audio_afe.c`

在 `audio_afe_processor_init()` 中用 `#ifndef CONFIG_SPIRAM` 跳过整个 AFE 初始化：

```c
OPERATE_RET audio_afe_processor_init(void)
{
#ifndef CONFIG_SPIRAM
    /* ESP32-C3/C6 等无 PSRAM 平台：AFE 需要 PSRAM 才能正常工作，
     * 跳过初始化，程序以 push-to-talk（手动 VAD）模式运行。 */
    ESP_LOGW(TAG, "No PSRAM – AFE processor skipped (use manual VAD / push-to-talk mode)");
    return OPRT_OK;
#else
    // ... 原有完整初始化 ...
#endif /* CONFIG_SPIRAM */
}
```

### 2. `boards/ESP32/common/audio/tdd_audio_8311_codec.c`

将 AFE 初始化失败改为非致命错误（只打 WARN，不返回错误），确保音频编解码器本身仍能正常工作：

```c
rt = audio_afe_processor_init();
if (rt != OPRT_OK) {
    /* 非致命：无 AFE 时 codec 仍可正常工作（push-to-talk 模式） */
    PR_WARN("audio_afe_processor_init failed (err:%d), VAD/wakeword disabled", rt);
    rt = OPRT_OK;
}
```

## 验证

修复后运行日志：

```
W (2130) audio_afe: No PSRAM – AFE processor skipped (use manual VAD / push-to-talk mode)
[01-01 00:00:01 ty D][tdd_audio_8311_codec.c:379] I2S 8311 read task args: 0x3fccbf20
```

程序不再崩溃，ESP32-C3 以 push-to-talk（手动 VAD）模式继续运行。

## 注意事项

- 无 PSRAM 平台跳过 AFE 后，**唤醒词检测**和**自动 VAD** 不可用
- 需在应用层使用 **手动 VAD 模式（mode 0）**，即按键触发录音
- 已在 `ai_default_config` 中将默认 `vad_mode` 设为 0（MANUAL）
