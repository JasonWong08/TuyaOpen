# 问题：运行时日志刷屏（`audio_afe: afe is not init`）

## 现象

程序启动后控制台被以下错误日志以每 10ms 一条的频率淹没，严重干扰其他日志的阅读：

```
E (2150) audio_afe: afe is not init
E (2160) audio_afe: afe is not init
E (2170) audio_afe: afe is not init
E (2180) audio_afe: afe is not init
...（持续不断）
```

## 根因

### 调用链

```
I2S 读任务 (esp32_i2s_8311_read_task) 每 ~10ms 循环一次
  → 采集一帧 PCM 数据
  → auio_afe_processor_feed(data, len)
    → if (false == sg_afe_proce.is_init)
        ESP_LOGE(TAG, "afe is not init");  ← 每帧都打错误日志
        return;
```

### 相关代码

`audio_afe.c` 中的原始实现：

```c
void auio_afe_processor_feed(uint8_t *data, int len)
{
    if(NULL == data || 0 == len) {
        ESP_LOGE(TAG, "param is err");    // 参数错误才需要日志
        return;
    }
    if(false == sg_afe_proce.is_init) {
        ESP_LOGE(TAG, "afe is not init"); // ← 每帧都触发，造成刷屏
        return;
    }
    // ...
}
```

AFE 在无 PSRAM 平台（ESP32-C3）被主动跳过（`is_init` 始终为 `false`），但 I2S 读任务仍以正常频率运行并持续调用 `feed`，导致错误日志每帧一条地打印。

## 修复

`platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/audio/audio_afe.c`

将 `is_init=false` 时的 `ESP_LOGE` 改为静默 `return`：

```c
void auio_afe_processor_feed(uint8_t *data, int len)
{
    if(NULL == data || 0 == len) {
        return;
    }
    if(false == sg_afe_proce.is_init) {
        /* AFE 未初始化（如无 PSRAM 平台），静默丢弃音频帧，
         * 避免 I2S 读任务以高频率刷写错误日志。 */
        return;
    }
    // ...
}
```

## 验证

修复后控制台不再出现 `audio_afe: afe is not init` 字样，其他业务日志恢复正常可读性。

## 说明

- 静默丢弃是正确行为：无 PSRAM 平台不支持 AFE 处理，丢弃音频帧不影响 push-to-talk 功能
- `param is err` 日志同样改为静默，因为在正常运行时该路径不应触发，若有需要可恢复
