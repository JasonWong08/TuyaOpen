# 问题：烧录后运行崩溃（ES8311 初始化 I2C NACK，触发 assert）

## 现象

固件启动后系统进入 AI/音频初始化阶段，日志出现大量 I2C 错误，最后崩溃并复位，关键片段：

- `I2C transaction unexpected nack detected`
- `I2C_If: Fail to write to dev 18`
- `ES8311: Open fail`
- `assert failed: codec_8311_init tdd_audio_8311_codec.c:256 (codec_if_ != NULL)`

## 根因（直接原因）

`boards/ESP32/common/audio/tdd_audio_8311_codec.c` 中：

```c
codec_if_ = es8311_codec_new(&es8311_cfg);
assert(codec_if_ != NULL);
```

当 ES8311 通过 I2C 初始化失败时，`es8311_codec_new()` 返回 `NULL`，触发 `assert`，导致系统崩溃。

## 根因（触发 I2C 失败的关键配置点）

### 1) ES8311 I2C 地址格式使用错误

本项目板级配置里曾将：

- `AUDIO_CODEC_ES8311_ADDR` 设为 `0x18`

但 TuyaOpen 依赖的 `esp_codec_dev` I2C 控制实现对地址的处理是：

```c
.device_address = (i2c_cfg->addr >> 1),
```

这意味着 `i2c_cfg->addr` 期望的是 **8-bit 地址格式**（即 7-bit 地址左移 1 位后的值）。

在 `esp_codec_dev` 里 ES8311 的默认地址即为：

- `ES8311_CODEC_DEFAULT_ADDR (0x30)`

因此对于 ES8311（7-bit 地址通常为 `0x18`），在此栈中应配置为：

- `AUDIO_CODEC_ES8311_ADDR = 0x30`

若误设为 `0x18`，则 `>> 1` 后变成 `0x0c`，会访问错误设备地址，必然导致 NACK。

### 2) I2S GPIO “not usable” 警告（需关注但不一定致命）

日志中还出现：

- `i2s_common: GPIO 13 is not usable, maybe conflict with others`
- `i2s_common: GPIO 12 is not usable, maybe conflict with others`

这通常表示这些 GPIO 在当前目标/Flash 配置下被标记为保留或存在复用冲突。即使 I2S 仍创建成功，也可能影响 ES8311 对 MCLK/BCLK 的依赖与稳定性，需要结合硬件与 SDK 配置进一步核对。

## 建议修复策略

### 必做：修正 ES8311 I2C 地址宏

在目标板 `board_config.h` 中将：

- `AUDIO_CODEC_ES8311_ADDR` 从 `0x18` 改为 `0x30`

涉及文件：

- `boards/ESP32/PETOI_ESP32C3_CUBE/board_config.h`

### 建议：将 assert 改为可恢复错误（增强鲁棒性）

对于量产/调试场景，建议将 `tdd_audio_8311_codec.c` 中的 `assert(codec_if_ != NULL)` 改为：

- 打印错误并返回失败（`OPRT_COM_ERROR` 等），避免硬件异常导致整机重启

涉及文件：

- `boards/ESP32/common/audio/tdd_audio_8311_codec.c`

## 验证

1. 修正地址后重新编译、烧录
2. 启动日志应不再出现针对 ES8311 的持续 NACK
3. 不再触发 `codec_8311_init` 的 assert

