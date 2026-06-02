# 2026-06-02 AI_BOARD USB Type-C CLI 与手柄事件调试记录

## 背景

当前硬件使用 `AI_BOARD`，固件烧写口和日志口都在 ESP32-S3 主板的 USB Type-C 端口上。最初在 VS Code Serial Monitor 中连接 `/dev/ttyACM0` 后，可以看到日志输出，但发送 `hello`、`quaddle_evt` 等 CLI 命令没有设备回显，说明 USB Type-C 口只有日志输出链路可用，CLI 输入没有进入 Tuya `tal_cli`。

调试目标：

- 保持 USB Type-C 作为日志和 CLI 输入口。
- 保持机器狗控制 UART 使用 `UART1 GPIO17/18`。
- 能通过 Serial Monitor 手动发送模拟手柄事件，例如 `quaddle_evt DPAD_U+`，并映射成机器狗 UART 指令。

## 本机与 VS Code 准备

确认本机同时存在 DEB/APT 版和 Snap 版 VS Code，最终保留当前 `code` 命令对应的 DEB/APT 版：

```text
/usr/bin/code
Version: 1.122.1
Commit: 8761a5560cfd65fdd19ce7e2bd18dab5c0a4d84e
```

安装 VS Code 扩展：

```text
ms-vscode.vscode-serial-monitor@0.13.1
```

Serial Monitor 使用配置：

```text
Port: /dev/ttyACM0
Baud Rate: 115200
Data Bits: 8
Stop Bits: 1
Parity: None
Flow Control: None
Line Ending: CRLF
```

## 问题定位

测试 `hello` 和 `quaddle_evt` 时，Serial Monitor 只能看到周期性日志：

```text
[ty I][tuya_main.c] Heap...
[ty I][app_chat_bot.c] Free heap size...
```

没有看到：

```text
helo world
quaddle_evt: accepted
```

结论：

- `/dev/ttyACM0` 的日志输出链路正常。
- 发送到 `/dev/ttyACM0` 的输入没有进入 `tal_cli`。
- `quaddle_evt` 功能本身不是第一问题，第一问题是 USB Type-C CLI RX 未打通。

进一步查看代码后确认：

- `tal_cli_init()` 默认使用 `TUYA_UART_NUM_0`。
- `second_uart` 使用 `UART1 GPIO17/18 @ 115200` 发机器狗指令。
- 原 ESP32 UART 适配里，USB Serial/JTAG 分支曾被设计成 only UART0，无法同时保留 UART1。
- 当前需求需要 `UART0 -> USB Serial/JTAG`，同时 `UART1 -> 普通 IDF UART GPIO17/18`。

## 代码修改

### 1. 平台 UART 适配

文件：

```text
platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_uart.c
```

主要调整：

- 增加 `TKL_UART0_USE_USB_SERIAL_JTAG` 判断。
- ESP32-S3 且启用 USB Serial/JTAG 时，`TUYA_UART_NUM_0` 使用 `driver/usb_serial_jtag`。
- `TUYA_UART_NUM_1` 继续使用 ESP-IDF `driver/uart`，保持 GPIO17/18 作为机器狗 UART。
- 为 USB Serial/JTAG 增加独立 RX task：`usb_jtag_rx`。
- USB Type-C 收到数据后调用原有 `uart_rx_cb[TUYA_UART_NUM_0]`，复用 `tal_uart` 的 ring buffer 和阻塞唤醒逻辑。
- 日志输出 `tal_uart_write(TUYA_UART_NUM_0, ...)` 仍通过 USB Serial/JTAG 发回电脑。

关键日志：

```text
USB_SERIAL_JTAG init done for UART0 CLI/log
```

### 2. AI_BOARD 配置

文件：

```text
apps/tuya.ai/your_chat_bot/app_default.config
apps/tuya.ai/your_chat_bot/config/AI_BOARD.config
boards/ESP32/AI_BOARD/Kconfig
```

保留的关键配置：

```text
CONFIG_ENABLE_ESP32S3_USB_JTAG_ONLY=y
CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y
```

说明：

- 当前硬件实际使用 `AI_BOARD`。
- 曾临时修改过 `DNESP32S3_BOX` 相关配置，后续已按确认还原。

### 3. DNESP32S3_BOX 修改已还原

已确认以下文件没有保留本次 USB/JTAG 配置修改：

```text
apps/tuya.ai/your_chat_bot/config/DNESP32S3_BOX.config
boards/ESP32/DNESP32S3_BOX/Kconfig
```

## 构建与验证

建议在板级 Kconfig 或平台适配改动后，执行干净构建：

```bash
rm -rf apps/tuya.ai/your_chat_bot/.build
rm -rf platform/ESP32/tuya_open_sdk/build
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
```

实际构建已通过：

```text
BUILD SUCCESS
Target    : your_chat_bot_QIO_1.0.1.bin
Output    : apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
Platform  : ESP32
Chip      : esp32s3
Board     : AI_BOARD
Framework : base
```

备注：

- `tools/check_format.py` 未成功运行，因为本机缺少 `clang-format`。
- `tos.py build` 完整编译通过，并确认 `tkl_uart.c` 重新编译成功。

## 串口验证结果

烧录新固件后，在 VS Code Serial Monitor 中发送：

```text
quaddle_evt DPAD_U+
quaddle_evt DPAD_D+
quaddle_evt DPAD_L+
quaddle_evt DPAD_R+
```

实际日志：

```text
quaddle_evt DPAD_U+
second_uart: TX UART1 GPIO17/18 3 bytes: "XL
"
quaddle_evt: accepted

quaddle_evt DPAD_D+
second_uart: TX UART1 GPIO17/18 3 bytes: "XS
"
quaddle_evt: accepted

quaddle_evt DPAD_L+
second_uart: TX UART1 GPIO17/18 3 bytes: "XD
"
quaddle_evt: accepted

quaddle_evt DPAD_R+
second_uart: TX UART1 GPIO17/18 3 bytes: "XG
"
quaddle_evt: accepted
```

验证结论：

- USB Type-C 的 CLI 输入已经打通。
- `quaddle_evt` 命令已注册并能被 Tuya CLI 执行。
- 模拟手柄事件能映射为机器狗 UART 指令。
- 指令已经通过 `UART1 GPIO17/18` 发送。

当前方向键映射：

```text
DPAD_U+ -> XL
DPAD_D+ -> XS
DPAD_L+ -> XD
DPAD_R+ -> XG
```

## 后续排查边界

如果后续看到：

```text
second_uart: TX UART1 GPIO17/18 ...
quaddle_evt: accepted
```

但机器狗没有动作，说明 Tuya CLI 到 UART1 这段已经正常，后续应排查：

- ESP32-S3 GPIO17/GPIO18 到机器狗控制板的接线。
- TX/RX 是否交叉连接。
- 两边是否共地。
- 机器狗端波特率是否为 `115200 8N1`。
- 机器狗端是否接受对应协议命令，例如 `XL`、`XS`、`XD`、`XG`、`khds`。

## 当前工作树注意

顶层仓库当前仍保留 AI_BOARD 相关配置修改：

```text
apps/tuya.ai/your_chat_bot/app_default.config
apps/tuya.ai/your_chat_bot/config/AI_BOARD.config
boards/ESP32/AI_BOARD/Kconfig
```

ESP32 平台目录是独立 Git 工作区，平台侧还有 UART 适配改动：

```text
platform/ESP32/tuya_open_sdk/tuyaos_adapter/src/drivers/tkl_uart.c
```

另外平台侧当前 `git status` 还显示：

```text
platform/ESP32/tuya_open_sdk/tuyaos_adapter/include/uart/tkl_uart.h
```

该头文件改动不是本记录中验证 USB Type-C CLI 的核心改动，提交前建议单独复核其 diff。
