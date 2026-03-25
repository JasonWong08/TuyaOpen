# 问题：ESP32-C3 构建失败（`GPIO_NUM_43/44` 未定义，UART0 默认引脚落到 ESP32-S3）

## 现象

在 ESP32-C3 内层 ESP-IDF 构建阶段编译 `tuyaos_adapter` 时，报错类似：

- `tkl_uart.c:244:30: error: 'GPIO_NUM_43' undeclared`
- `tkl_uart.c:245:30: error: 'GPIO_NUM_44' undeclared`

## 根因

`platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_uart.c` 中 UART0 默认分支写死为 ESP32-S3 的引脚：

- 若未定义 `UART_NUM0_TX_PIN`/`UART_NUM0_RX_PIN`，就回退到 `GPIO_NUM_43/44`
- ESP32-C3 不存在 GPIO 43/44，导致编译失败

同时该文件**并不会** `#include "board_config.h"`，所以即使板级头文件里定义了 `UART_NUM0_TX_PIN`/`UART_NUM0_RX_PIN`，也不会生效。

## 修复

在 `tkl_uart.c` 的默认分支中增加 ESP32-C3（以及同类 C6）的专用回退：

- ESP32-C3/C6 UART0 默认：TX=GPIO21，RX=GPIO20
- 仍保留原有“若定义了 `UART_NUM0_TX_PIN`/`UART_NUM0_RX_PIN` 则优先使用”的逻辑

涉及文件：

- `platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_uart.c`

## 验证

- 重新执行 `tos.py build`
- 确认内层 ESP-IDF 构建不再出现 `GPIO_NUM_43/44` 未定义错误，并可继续链接/出固件

