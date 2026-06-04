# 2026-06-03 Quaddle BLE HID 手柄调试与代码修改记录

## 背景

本次继续推进 Arduino 项目：

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame/QuaddleGame.ino
```

向当前 TuyaOpen 项目的整合：

```text
apps/tuya.ai/your_chat_bot
```

目标是让 BM769 / GamepadSpace-Q34B 蓝牙手柄通过 TuyaOpen / ESP-IDF BLE Central 接入，并把手柄输入解码成 Arduino 兼容事件文本，再调用：

```c
quaddle_robot_bridge_handle_line(...)
```

最终通过当前工程已有的机器狗 UART1 通道输出动作指令：

```text
UART1 TX GPIO17
UART1 RX GPIO18
Baudrate 115200
```

## 昨日现象汇总

### log07

串口日志显示手柄并不是完全没有连接成功：

```text
target adv name=GamepadSpace-Q34B
connected handle=3
svc uuid=0x1812
input char uuid=0x2a4d handle=23
input char uuid=0x2a4d handle=27
input char uuid=0x0003 handle=46
input char uuid=0x0003 handle=52
ready, subscribed 4 input report(s)
```

但订阅完成后开始出现大量：

```text
GATT read failed handle=... rc=6
```

判断：

- BLE 连接、服务发现、CCCD 订阅已经完成。
- 问题不是“完全没连上”，而是订阅后立即以 25ms 周期轮询读所有 input report。
- Tuya NimBLE 同一连接上的 GATT procedure 不能这样重叠发起。
- 轮询 read 与 HID wake 写入、上一次 read 互相抢占，导致 `rc=6`。

### log08

修复 `rc=6` 后重新烧录，日志显示：

```text
ready, subscribed 4 input report(s), readable 2
set report protocol mode handle=21
exit suspend handle=35
read polling enabled every 100ms (2 readable input report(s))
```

并且没有继续大量出现 `rc=6`。

但是：

- 手柄蓝灯仍然一直闪烁。
- 没有看到 `receive notify ok`。
- 摇杆输入没有触发机器狗动作。

判断：

- GATT 轮询重叠问题已解决。
- 当前更像是手柄没有进入它认可的“已配对 / 已加密主机”状态。
- Arduino 版本在连接后调用了 `NimBLEDevice::startSecurity(...)`，TuyaOpen 版本之前缺少对应流程。

## 主要代码修改

### 1. BLE HID Central 接入工程

相关文件：

```text
apps/tuya.ai/your_chat_bot/include/quaddle_ble_hid_central.h
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
apps/tuya.ai/your_chat_bot/CMakeLists.txt
apps/tuya.ai/your_chat_bot/Kconfig
apps/tuya.ai/your_chat_bot/app_default.config
apps/tuya.ai/your_chat_bot/src/app_chat_bot.c
```

当前配置：

```text
CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL=y
CONFIG_QUADDLE_BLE_GAMEPAD_NAME_FILTER="bm769,q34b,gamepadspace"
```

初始化路径：

```c
quaddle_robot_bridge_init();
quaddle_ble_hid_central_init();
```

当前 BLE Central 主要流程：

1. 扫描目标手柄广播名。
2. 连接 `BM769` / `Q34B` / `GamepadSpace`。
3. 发现 GATT 服务和 characteristic。
4. 订阅 HID Report `0x2A4D` 和厂商 input characteristic。
5. 对 HID protocol mode / control point 做 wake 初始化。
6. 接收 notify 或 read report。
7. 解码成 Arduino 风格事件文本。
8. 调用 `quaddle_robot_bridge_handle_line()`。

## log07 对应修复

### 1. GATT read 改成单飞模式

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

修改点：

- 将轮询周期从 `25ms` 调整为 `100ms`。
- 增加 `poll_read_pending`。
- 只有前一个 read 完成后，才允许发起下一个 read。
- 断开、清配对、重连时重置 pending 状态。

目的：

- 避免多个 `ble_gattc_read()` 在同一连接上重叠。
- 避免刷屏 `GATT read failed ... rc=6`。

### 2. 只轮询具备 READ 属性的 input report

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
src/tal_bluetooth/nimble/tkl_bluetooth.c
tools/porting/adapter/bluetooth/tkl_bluetooth_def.h
platform/ESP32/tuya_open_sdk/tuyaos_adapter/include/bluetooth/tkl_bluetooth_def.h
```

修改点：

- `TKL_BLE_GATT_CHAR_HANDLE_T` 增加 `property` 字段。
- NimBLE characteristic discovery 中透传 `chr->properties`。
- 应用层记录每个 input characteristic 的 `prop=0x..`。
- read polling 只选择带 `TKL_BLE_GATT_CHAR_PROP_READ` 的 input report。

log08 中验证到：

```text
input char uuid=0x2a4d handle=23 end=26 prop=0x12
input char uuid=0x2a4d handle=27 end=30 prop=0x12
input char uuid=0x0003 handle=46 end=47 prop=0x10
input char uuid=0x0003 handle=52 end=53 prop=0x10
ready, subscribed 4 input report(s), readable 2
```

含义：

- HID `0x2A4D` 两个 report 是 READ + NOTIFY。
- 厂商 `0x0003` 两个 report 是 NOTIFY only。
- 当前只轮询前两个，后两个只等 notify。

### 3. GATT read 失败也回调上层

文件：

```text
src/tal_bluetooth/nimble/tkl_bluetooth.c
```

修改点：

- 新增 `tuya_ble_gattc_read_callback()`。
- read 成功时透传 `TKL_BLE_GATT_EVT_READ_RX`。
- read 失败时也回调一次 `TKL_BLE_GATT_EVT_READ_RX`，但 `result` 为失败状态且不带数据。

目的：

- 让上层释放 `poll_read_pending`。
- 避免一次 ATT error 后永远卡住 read pending。

## log08 对应修复

### 1. 增加 BLE Security / Pairing 流程

Arduino 代码连接后会执行：

```cpp
NimBLEDevice::setSecurityAuth(true, false, true);
NimBLEDevice::setSecurityIOCap(3);
NimBLEDevice::startSecurity(client_->getConnId());
```

TuyaOpen 版本之前没有主动发起 security，因此手柄可能允许连接和 GATT 发现，但不进入已配对可用状态。

本次新增：

```text
tools/porting/adapter/bluetooth/tkl_bluetooth.h
platform/ESP32/tuya_open_sdk/tuyaos_adapter/include/bluetooth/tkl_bluetooth.h
src/tal_bluetooth/nimble/tkl_bluetooth.c
```

新增接口：

```c
OPERATE_RET tkl_ble_gap_security_request(uint16_t conn_handle);
```

内部调用：

```c
ble_gap_security_initiate(conn_handle);
```

### 2. 打开 Legacy bonding 配置

文件：

```text
src/tal_bluetooth/CMakeLists.txt
```

在启用 `CONFIG_ENABLE_QUADDLE_BLE_HID_CENTRAL` 时增加：

```text
TY_HS_BLE_ROLE_CENTRAL=1
TY_HS_BLE_SM_LEGACY=1
TY_HS_BLE_SM_BONDING=1
TY_HS_BLE_SM_IO_CAP=3
TY_HS_BLE_SM_OUR_KEY_DIST=3
TY_HS_BLE_SM_THEIR_KEY_DIST=3
```

说明：

- 使用 Legacy Just Works + bonding。
- 不启用 Secure Connections，避免额外 ECC / CMAC 依赖。
- IO capability 使用 NoInputNoOutput，与 Arduino 项目中的 `setSecurityIOCap(3)` 对齐。

### 3. 连接后先 security，再 GATT discovery

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

修改前：

```text
connect success -> MTU exchange -> service discovery -> subscribe
```

修改后：

```text
connect success
-> tkl_ble_gap_security_request()
-> wait 650ms
-> MTU exchange
-> service discovery
-> subscribe
```

新增关键日志：

```text
Start GAP security handle=...
quaddle ble hid: security requested, wait 650ms before GATT discovery
quaddle ble hid: security wait done, start GATT discovery
```

## 为 BLE Security 补齐的 Tuya NimBLE 兼容头

打开 Legacy SM 后，Tuya NimBLE 会编译 `ble_sm_alg.c`，但当前仓库缺少几个上游 NimBLE / tinycrypt 兼容头。

新增文件：

```text
src/tal_bluetooth/nimble/include/ble.h
src/tal_bluetooth/nimble/include/mesh_crypt/aes.h
src/tal_bluetooth/nimble/include/mesh_crypt/constants.h
src/tal_bluetooth/nimble/include/mesh_crypt/utils.h
```

说明：

- `ble.h` 是轻量聚合头，包含 `tuya_ble.h`、`ble_hs.h`、`ble_gap.h`、`ble_gatt.h`、`ble_sm.h`、`ble_uuid.h`。
- `mesh_crypt/aes.h` 只提供 `ble_sm_alg.c` Legacy pairing 所需的 tinycrypt AES 接口。
- AES 后端使用项目已有的 mbedTLS AES-128 ECB。
- `constants.h` 提供 `TC_CRYPTO_SUCCESS` / `TC_CRYPTO_FAIL`。
- `utils.h` 复用已有 `ble_endian.h` 中的 `swap_buf()` 等工具。

注意：

- 第一次新增 `ble.h` 时，include guard 与 `tuya_ble.h` 同名，导致 `ble_addr_t` 没有被定义。
- 已修复为独立 guard：`H_TUYA_NIMBLE_BLE_AGGREGATE_`。

## 清除配对入口

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

已保持与 Arduino 项目一致的入口习惯：

```text
r
quaddle_ble_clear
BOOT GPIO0 长按 2000ms
```

清除内容：

- 停止扫描 / 连接 / 订阅 / 轮询定时器。
- 如正在连接则 cancel。
- 如已连接则 disconnect。
- 调用 `tkl_ble_gap_unpair_all()` 清除 bonding。
- 删除 KV 中保存的手柄地址和名称。
- 重置解码状态和机器狗桥接状态。
- 重新开始扫描。

## 构建记录

最终构建通过：

```text
BUILD SUCCESS
Target    : your_chat_bot_QIO_1.0.1.bin
Output    : apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
Platform  : ESP32
Chip      : esp32s3
Board     : AI_BOARD
Framework : base
```

固件路径：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1/your_chat_bot_QIO_1.0.1.bin
```

构建过程中遇到过两类问题：

- 沙盒环境下 `.git/modules/.../config` 只读，`tos.py build` 需要提权执行。
- 打开 BLE SM 后缺少 `ble.h` 和 tinycrypt 兼容头，已补齐。

## 当前状态

截至 2026-06-03 结束时：

- BLE Central 可以扫描并连接 `GamepadSpace-Q34B`。
- 可以发现 HID `0x1812` 和厂商服务。
- 可以识别并订阅 4 个 input report。
- `rc=6` GATT read 重叠问题已修复。
- 已补上 security / pairing 发起流程。
- 已完成一次成功构建。

仍需上板验证：

- 手柄蓝灯是否从闪烁变为稳定连接状态。
- 是否出现 `receive notify ok`。
- 摇杆 / 按键是否能触发 `[Q34B] ...` 或 `[BM769] ...` 事件。
- 事件是否最终进入 `quaddle_robot_bridge_handle_line()` 并通过 UART1 GPIO17/18 驱动机器狗。

## 下一步建议

烧录最新固件后，优先观察串口中的这些日志：

```text
Start GAP security handle=...
security requested, wait 650ms before GATT discovery
security wait done, start GATT discovery
ready, subscribed ... input report(s)
receive notify ok, handle=... len=...
quaddle ble hid: report handle=... len=...
```

如果仍然蓝灯闪烁且没有 notify：

- 重点看 `Start GAP security ... rc=...` 返回码。
- 看是否出现 pairing fail、encryption change、disconnect reason。
- 尝试先通过 `r` 或 BOOT 长按清除旧 bonding，再让手柄重新进入配对。

如果能收到 notify 但机器狗无动作：

- 下一步应抓取 notify 原始报文长度和前几个字节。
- 对照 Arduino `Bm769GamepadBle.cpp` 中 Q34B / BM769 解码逻辑调整 TuyaOpen 侧报文解析。

