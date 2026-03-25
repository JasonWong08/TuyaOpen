# 问题：ESP32-C3 构建失败（`tkl_i2s.c` 中 `sg_i2s_gpio_cfg` 未定义）

## 现象

ESP32-C3 内层 ESP-IDF 构建报错类似：

- `tkl_i2s.c:96:26: error: 'sg_i2s_gpio_cfg' undeclared (first use in this function)`

## 根因

`platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_i2s.c` 里，`sg_i2s_gpio_cfg` 仅在 `CONFIG_IDF_TARGET_ESP32S3` 条件下定义：

- ESP32-C3 目标编译时该数组没有定义
- 但 `tkl_i2s.c` 仍会被编译（例如启用 `CONFIG_ENABLE_AUDIO` / `CONFIG_ENABLE_I2S`）

## 修复

为 ESP32-C3（以及同类 C6）补齐 `sg_i2s_gpio_cfg` 的定义，提供合理的默认引脚，保证文件可编译通过。

涉及文件：

- `platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_i2s.c`

## 备注

在本项目中 ES8311 音频编解码器路径使用的是 `boards/ESP32/common/audio/tdd_audio_8311_codec.c`，其内部使用 ESP-IDF I2S 标准驱动直接配置 `mclk/bclk/ws/dout/din`，因此这里的默认数组更多是**编译期兜底**。

## 验证

- `tos.py build` 可继续推进，不再出现 `sg_i2s_gpio_cfg` 未定义错误

