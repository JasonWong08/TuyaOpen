# 问题：链接失败（`mbedtls_hkdf` 未定义引用）

## 现象

ESP32-C3 内层 ESP-IDF 链接阶段报错类似：

- `undefined reference to 'mbedtls_hkdf'`

常见调用栈来源于 Tuya AI 协议层（例如 `tuya_ai_protocol.c` 生成签名/加密 key 的路径）。

## 根因

`mbedtls_hkdf` 对应 mbedTLS 的 HKDF 模块，需在 ESP-IDF 的 mbedTLS 配置中启用 `MBEDTLS_HKDF_C`。

在本项目使用的 ESP32-C3 默认 `sdkconfig` 变体中：

- `CONFIG_MBEDTLS_HKDF_C` 处于禁用状态

因此 mbedTLS 没有编译进 HKDF 实现，导致最终链接失败。

## 修复

在 ESP32-C3 16MB Flash 的 sdkconfig 默认文件中启用：

- `CONFIG_MBEDTLS_HKDF_C=y`

涉及文件：

- `platform/ESP32/tuya_open_sdk/sdkconfig_esp32c3_16m`

同时需要清理/重生成 `sdkconfig`（避免旧配置残留），确保 `idf.py set-target` 后生成的 `sdkconfig` 生效。

## 验证

- 重新构建 `tos.py build` 通过链接阶段
-（可选）检查生成的 `sdkconfig`/`sdkconfig.h` 中存在 `CONFIG_MBEDTLS_HKDF_C 1`

