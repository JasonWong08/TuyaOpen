# UART1 第二串口测试（hello, world.）

每秒经 **UART1** 发送一行 `hello, world.`，并回显收到的数据，用于验证 DNESP32S3_BOX 扩展 4Pin 串口（默认 **TX=GPIO17, RX=GPIO18**）。

## 与 USB 日志口的区别

| 用途 | 硬件 UART | 典型 GPIO (ESP32-S3) |
|------|-----------|----------------------|
| USB 烧录/日志 | UART0 | 43 / 44 |
| 本例程测试口 | UART1 | 17 / 18 |

## 编译与烧录

```bash
cd /path/to/TuyaOpen
. ./export.sh
cd examples/peripherals/uart1_second_hello
tos.py build
tos.py flash
tos.py monitor
```

板型请选择 **DNESP32S3_BOX**（`app_default.config` 已写好，或 `tos.py config choice` 选 `config/DNESP32S3_BOX.config`）。

## 接线验证

1. **USB-TTL**：GND 共地；TTL **RX** 接板子 **GPIO17（TX）**；115200 8N1。
2. 上电后 PC 串口应每秒收到：`hello, world.`
3. 在 PC 串口发送任意字符，板子 USB 日志会打印 `UART1 RX ...`，并向 UART1 回发 `echo: ...`。

注意：本例程 UART 使用**非阻塞读**；若使用 `O_BLOCK` 且无接收数据，`tal_uart_read` 会一直阻塞，表现为只发出第一行 hello。

若 GPIO17 无波形，可对调 TX/RX，或测量 **GPIO43** 是否误接到扩展座（说明座子不是 UART1）。

## 修改引脚

编辑 `src/example_uart1_second.c` 顶部宏：

- `UART1_TEST_TX_PIN` / `UART1_TEST_RX_PIN`
- `UART1_TEST_BAUD`
