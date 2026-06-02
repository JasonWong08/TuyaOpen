# 2026-06-02 Quaddle 手柄项目整合进展

## 背景

本次目标是把 Arduino 项目：

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame/QuaddleGame.ino
```

整合到当前 TuyaOpen AI 对话模块：

```text
apps/tuya.ai/your_chat_bot
```

两个项目都运行在 ESP32-S3 主板上，并且都通过第二串口向机器狗主板发送控制指令。当前工程中的机器狗控制串口为：

```text
UART1
TX: GPIO17
RX: GPIO18
Baudrate: 115200
Format: 8N1
```

## 干扰风险分析

如果 Arduino 手柄逻辑和当前 AI 对话逻辑各自初始化串口、各自直接写 UART1，就会有干扰风险。

主要风险如下：

- 物理层面：同一块 ESP32-S3 上 GPIO17/18 只能作为一个 UART 通道使用，不能让两套独立串口驱动同时控制。
- 字节层面：AI 指令和手柄指令如果从不同任务同时写 UART，可能造成字节交错，例如两个命令混在一起发给机器狗。
- 逻辑层面：AI 对话和手柄都能控制机器狗时，可能出现“最后一个指令生效”的控制权覆盖。例如 AI 刚发出休息命令，手柄又立刻发出行走命令。

本次已解决前两个问题：

- 所有机器狗串口输出统一走 `second_uart`。
- `second_uart` 写入增加互斥锁，避免多任务同时写 UART1 导致字节交错。

逻辑层面的控制权仲裁还需要后续进一步设计，例如手柄优先、AI 优先、最近输入优先、模式切换或超时释放。

## 当前整合策略

Arduino 项目的 BLE/NimBLE 层不能直接原样放入当前工程，因为它依赖 Arduino Core：

```text
Arduino.h
HardwareSerial
NimBLEDevice
Print
```

当前工程是 TuyaOpen/ESP-IDF 构建体系，因此本次没有直接编译 `.ino` 文件，而是先移植核心映射层：

```text
手柄事件文本 -> 机器狗串口命令
```

例如：

```text
A+              -> khds
X+              -> kup
DPAD_U+         -> XL
DPAD_D+         -> XS
DPAD_L+         -> XD
DPAD_R+         -> XG
LSTICK 20,-3    -> 对应左摇杆区域命令
RSTICK 30,-20   -> Jr 二进制帧
```

后续真实蓝牙手柄接入时，只需要把 BLE HID 报文解码成类似 Arduino 输出的事件文本，再调用：

```c
quaddle_robot_bridge_handle_line("[BM769] LSTICK 20,-3");
```

或：

```c
quaddle_robot_bridge_handle_line("A+");
```

## 代码修改概览

### 1. 第二串口增加互斥保护

文件：

```text
apps/tuya.ai/your_chat_bot/src/second_uart.c
apps/tuya.ai/your_chat_bot/include/second_uart.h
```

主要修改：

- 引入 `tal_mutex.h`。
- 增加 `s_tx_mutex`。
- 在 `second_uart_init()` 中创建 mutex。
- 在 `tal_uart_write()` 前后加锁/解锁。
- 保留原有重复命令去重逻辑。
- 新增 force 发送接口，用于手柄场景中必须允许重复发送的命令。

新增接口：

```c
OPERATE_RET second_uart_send_data_force(const uint8_t *data, size_t length);
OPERATE_RET second_uart_send_string_force(const char *str);
```

说明：

- 普通 AI/语音命令继续使用 `second_uart_send_string()`，会保留重复去重。
- 手柄中的 `c`、`d`、DPAD、`Jr` 二进制帧等需要重复发送或精确发送的场景使用 force 接口。

### 2. 新增 Quaddle 手柄桥接模块

文件：

```text
apps/tuya.ai/your_chat_bot/include/quaddle_robot_bridge.h
apps/tuya.ai/your_chat_bot/src/quaddle_robot_bridge.c
```

该模块负责把 Arduino 风格手柄事件映射成机器狗 UART 命令。

公开接口：

```c
OPERATE_RET quaddle_robot_bridge_init(void);
void quaddle_robot_bridge_reset(void);
void quaddle_robot_bridge_poll(void);
OPERATE_RET quaddle_robot_bridge_handle_line(const char *line);
```

已移植的主要逻辑：

- ABXY 按键映射。
- L1/R1/ZL/ZR 按键映射。
- DPAD 方向键映射。
- 左摇杆分区映射。
- 左摇杆结合 ABXY/ZR 的组合命令映射。
- 右摇杆 `RSTICK` 到 `Jr` 二进制帧映射。
- `k*` 命令去重。
- ABXY 单独按键 200ms 延迟判断。
- 左摇杆中间区域/小幅移动 100ms 延迟判断。
- 20ms 周期 poll，用于处理延迟发送逻辑。

### 3. 接入应用初始化

文件：

```text
apps/tuya.ai/your_chat_bot/src/app_chat_bot.c
```

在机器狗第二串口功能启用时，同时初始化：

```c
robot_uart_voice_init();
quaddle_robot_bridge_init();
```

这样当前 AI 对话串口控制和手柄桥接层共享同一个 `second_uart` 出口。

### 4. CMake 构建接入

文件：

```text
apps/tuya.ai/your_chat_bot/CMakeLists.txt
```

修改点：

- 将 `quaddle_robot_bridge.c` 从默认源文件列表中过滤掉。
- 仅在 `CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y` 时加入编译。

这样手柄桥接层跟随机器狗第二串口功能启用，不额外引入新的 Kconfig 开关。

## CLI 模拟测试

为了在真实 BLE 手柄接入前验证映射逻辑，新增了 CLI 命令：

```text
quaddle_evt
```

示例：

```text
quaddle_evt A+
quaddle_evt B+
quaddle_evt X+
quaddle_evt Y+
quaddle_evt LSTICK 20,-3
quaddle_evt RSTICK 30,-20
quaddle_evt DPAD_U+
quaddle_evt DPAD_D+
quaddle_evt DPAD_L+
quaddle_evt DPAD_R+
```

正常情况下会看到：

```text
quaddle_evt: accepted
```

并能在日志中看到第二串口发送记录，例如：

```text
second_uart: TX UART1 GPIO17/18 3 bytes: "XL
"
```

说明：

- CLI 串口用于输入模拟手柄事件。
- 实际机器狗控制命令仍然通过 UART1 GPIO17/18 发出。
- 烧录、日志和 CLI 通常走 ESP32-S3 的 UART0/USB 通道。
- 机器狗控制串口走 UART1 GPIO17/18。

## USB Type-C CLI 调试补充

今天还确认并修复了 AI_BOARD 上 USB Type-C CLI 输入链路问题。

现状：

- USB Type-C 可用于固件烧录。
- USB Type-C 可用于日志输出。
- USB Type-C 可用于 Tuya CLI 输入。
- UART1 GPIO17/18 保持给机器狗主板使用。

相关详细记录见：

```text
apps/tuya.ai/your_chat_bot/2026-06-02_usb_jtag_cli_worklog.md
```

## 构建与验证

执行过构建：

```bash
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
```

构建结果：

```text
BUILD SUCCESS
Target    : your_chat_bot_QIO_1.0.1.bin
Output    : apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
Platform  : ESP32
Chip      : esp32s3
Board     : AI_BOARD
Framework : base
```

格式检查：

```bash
python3 tools/check_format.py --debug --files ...
```

未能运行成功，原因是当前环境缺少：

```text
clang-format
```

构建本身已通过。

## 当前完成状态

已完成：

- 分析 AI 对话模块和 Arduino 手柄项目共用 UART1 GPIO17/18 的干扰风险。
- 将机器狗串口发送统一收敛到 `second_uart`。
- 为 `second_uart` 增加互斥锁，避免并发写导致串口数据交错。
- 移植 Quaddle 手柄事件到机器狗指令的核心映射逻辑。
- 增加 CLI 模拟入口 `quaddle_evt`。
- 接入应用初始化和 CMake 构建。
- 验证 ESP32-S3 AI_BOARD 构建通过。
- 验证 CLI 模拟事件可以映射并通过 UART1 GPIO17/18 发送。

未完成：

- 尚未接入真实 BLE 手柄扫描、连接、订阅和 HID 报文解析。
- 尚未实现 AI 与手柄之间的控制权仲裁策略。
- 尚未在真实手柄连接场景下做完整联调。

## 后续建议

下一阶段建议按以下顺序推进：

1. 使用 ESP-IDF `esp_hid` 或 Tuya/ESP-IDF BLE 接口实现 BLE HID Central。
2. 扫描并连接目标游戏手柄。
3. 订阅 HID Input Report。
4. 将 HID 报文解码成 Arduino 兼容事件文本。
5. 调用 `quaddle_robot_bridge_handle_line()`。
6. 增加 AI/手柄控制权策略，例如手柄输入后 500ms 内优先手柄，或通过显式模式切换控制。
7. 使用真实机器狗主板验证 UART1 GPIO17/18 的动作效果。

