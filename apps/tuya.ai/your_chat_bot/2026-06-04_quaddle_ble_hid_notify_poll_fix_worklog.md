# 2026-06-04 Quaddle BLE HID Notify 接收与回中误发修复记录

## 背景

本次继续基于前两天记录推进：

```text
apps/tuya.ai/your_chat_bot/2026-06-02_quaddle_gamepad_integration_summary.md
apps/tuya.ai/your_chat_bot/2026-06-03_quaddle_ble_hid_debug_worklog.md
```

目标仍然是把 Arduino 参考项目：

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame
```

中的 BM769 / GamepadSpace-Q34B BLE 手柄流程整合到当前 TuyaOpen 工程：

```text
apps/tuya.ai/your_chat_bot
```

并最终通过：

```c
quaddle_robot_bridge_handle_line(...)
```

将手柄事件映射成机器狗 UART1 指令。

## 今日主要进展

### 1. 手柄连接、配对、GATT 订阅已跑通

今日之前的状态是：

- 手柄可以连接或部分连接。
- 有时手柄蓝灯常亮并震动。
- 但 TuyaOpen 串口监视器看不到 HID notify 数据。
- 摇杆动作无法进入 `quaddle_robot_bridge_handle_line()`。

今日修复后，TuyaOpen 固件已经可以：

- 连接 `GamepadSpace-Q34B`。
- 完成 GAP security / pairing。
- 发现 HID `0x1812` 服务。
- 订阅 HID Report `0x2A4D`。
- 收到 10 字节 Q34B HID notify。
- 解码出 `[Q34B] LSTICK ...` 事件。
- 通过 UART1 GPIO17/18 控制机器狗动作。

验证日志中已经能看到：

```text
quaddle ble hid: central init fix=20260604-att-notify-rx-v7
quaddle ble hid: notify handle=27 len=10 data=...
[Q34B] LSTICK ...
second_uart: TX UART1 GPIO17/18 ...
```

### 2. Arduino 参考工程增加 BLE 流程对照日志

为了确认 Arduino 项目中成功连接手柄的真实流程，在参考项目：

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame/Bm769GamepadBle.cpp
```

增加了串口调试日志，重点输出：

- BLE 初始化、security 配置。
- scan / connect / startSecurity 流程。
- vendor shard service / char 发现与订阅。
- HID `0x1812` / `0x2A4D` 发现与订阅。
- notify 原始字节。
- Q34B report 路由和帧偏移。
- canonical 摇杆/按钮状态。
- 最终 emit 的 Arduino 兼容事件文本。
- 事件文本对应的机器狗 UART 命令预览。

Arduino 成功日志确认：

```text
[BLE_FLOW] notify uuid=0x2a4d len=10 vendor=0 count=0
[BLE_FLOW] notify-hid len=10 data=7C 80 80 80 FF 00 00 00 00 8F
[BLE_FLOW] feed report len=10 profile q34b->q34b q34b=1 bm769_hdr=0
[BLE_FLOW] q34b frame offset=0 len=10 strict=1 profile=q34b
[BLE_FLOW] canonical tag=[Q34B] raw=124,128,128,128 axis=-4,0,0,0 buttons=0x000 hat=0x0 prev=1 uart=1 print=1
[Q34B] LSTICK 1,12    kwkF
[BLE_FLOW] emit line="[Q34B] LSTICK 1,12" preview="kwkF" uart=1
```

结论：

- Q34B 的真实摇杆数据来自 HID Report `0x2A4D` notify。
- 报文通常是 10 字节，最后一字节为 `0x8F`。
- 当前 Q34B 数据不需要跳过 HID report id，offset 为 `0`。

## 今日问题一：TuyaOpen 收不到 notify

### 现象

TuyaOpen 固件连接手柄后已经能完成 service / characteristic discovery，并订阅：

```text
input char uuid=0x2a4d handle=23 end=26 prop=0x12
input char uuid=0x2a4d handle=27 end=30 prop=0x12
input char uuid=0x0003 handle=46 end=47 prop=0x10
input char uuid=0x0003 handle=52 end=53 prop=0x10
ready, subscribed 4 input report(s), readable 2
```

但拨动摇杆时看不到 notify 回调，底层日志反复出现：

```text
Rx Op Not Support= 0x1b
```

### 原因分析

`0x1b` 是 ATT Handle Value Notification。

TuyaOpen 当前使用的 NimBLE `ble_att.c` 中 ATT 接收分发表有明确注释：

```c
/** Dispatch table for incoming ATT commands.  Must be ordered by op code. */
```

但双角色编译时，central 相关 opcode 和 peripheral 相关 opcode 分块排列，导致表整体不是严格按 opcode 升序。

查找函数遇到：

```c
if (entry->bde_op > op) {
    break;
}
```

会提前停止。因此当收到 `0x1b` notify 时，表中前面已经出现了更大的 opcode，导致 notify 没被找到，最终打印：

```text
Rx Op Not Support= 0x1b
```

这解释了：

- 手柄已经震动、蓝灯常亮。
- GATT 订阅完成。
- 但摇杆 notify 无法进入上层。

### 修复

文件：

```text
src/tal_bluetooth/nimble/host/ble_att.c
```

修改：

- 将 ATT dispatch table 重排为全局 opcode 升序。
- `BLE_ATT_OP_NOTIFY_REQ` `0x1b` 放在 `0x1d`、`0x1e` 前。
- 同时兼顾 central / peripheral 双角色编译，避免其它 opcode 也被提前截断。

修复后启动指纹：

```text
quaddle ble hid: central init fix=20260604-att-notify-rx-v7
```

验证结果：

- TuyaOpen 能收到 `notify handle=... len=10 data=...`。
- 摇杆动作能生成 `[Q34B] LSTICK ...`。
- UART1 能发出机器狗动作指令。

## 今日问题二：摇杆回中后仍持续发送串口指令

### 现象

修复 notify 后，手柄已经能控制机器狗动作，但摇杆回中、不操作手柄时，UART1 仍持续输出动作指令。

TuyaOpen 日志中出现重复假事件：

```text
[Q34B] LSTICK -128,128
[Q34B] RSTICK -100,128
[Q34B] DPAD_U+
second_uart: TX UART1 GPIO17/18 ...
```

同时也能看到正常的回中 notify：

```text
quaddle ble hid: notify handle=27 len=10 data=80 7C 80 80 FF 00 00 00 00 8F
[Q34B] LSTICK 0,4
```

### 原因分析

Arduino 参考工程中 read polling 逻辑是：

```cpp
bool changed = (len != 0) && (memcmp(v.data(), prev, len) != 0);
if (changed) {
  feedReport_(v.data(), len);
  memcpy(prev, v.data(), len);
}
```

即：

- read 值没变化，不喂给解码器。
- 只有变化的 read report 才进入 `feedReport_()`。

TuyaOpen 版本之前是：

```c
if (read success && len > 0) {
    feed_report(data, len);
}
```

这导致周期 read polling 会把某些 readable HID report 的全 0 数据持续送进 Q34B 解码。

而 TuyaOpen Q34B 兜底逻辑中：

```c
if (s_hid.profile == GP_PROFILE_Q34B && len >= 10 && !looks_like_bm769(d, len)) {
    feed_canonical("[Q34B]", d[0], d[1], d[2], d[3], ...);
}
```

会把全 0 read 数据误认为 Q34B frame：

```text
00 00 00 00 00 00 00 00 00 00
```

于是被解析为：

```text
lx = 0   -> -128
ly = 0   -> 128
rx = 0   -> -128, RSTICK 限幅为 -100
f4 = 0   -> DPAD_U
```

最终产生假的摇杆、右摇杆和方向键事件。

### 修复

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

修改：

1. 增加 read polling 快照：

```c
uint8_t poll_snap[QUADDLE_BLE_MAX_INPUT_CHARS][64];
uint8_t poll_snap_len[QUADDLE_BLE_MAX_INPUT_CHARS];
```

2. 增加 all-zero read report 过滤：

```c
report_all_zero(...)
```

3. 增加 handle -> input char index 查询：

```c
input_char_index_by_handle(...)
```

4. read 回调改成：

- 全 0 read 忽略。
- 与上一次 read 快照完全相同则忽略。
- 只有变化且非全 0 的 read report 才 `feed_report()`。

5. 更关键的是：如果已经存在 notify / indicate input characteristic，则跳过 read polling。

当前 Q34B 已经确认真实数据来自 notify，因此 read polling 只作为没有 notify 时的兜底。

新日志会显示：

```text
quaddle ble hid: read polling skipped, readable=2 notify=4
```

修复后启动指纹：

```text
quaddle ble hid: central init fix=20260604-poll-filter-v8
```

## 今日问题三：摇杆中位死区与 Arduino 不一致

### 现象

Q34B 回中时可能出现轻微偏移：

```text
[Q34B] LSTICK 0,4
```

Arduino 映射中认为 `|lx|` 和 `|ly|` 都小于等于 `5` 是中位：

```text
LSTICK_DEAD_ABS_MAX 5
```

TuyaOpen 当前 bridge 中是：

```c
#define LSTICK_DEAD_ABS_MAX 3
if (ax < LSTICK_DEAD_ABS_MAX && ay < LSTICK_DEAD_ABS_MAX)
```

这会让 `0,4` 这种轻微偏移不被当成中位。

### 修复

文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_robot_bridge.c
```

修改为与 Arduino 对齐：

```c
#define LSTICK_DEAD_ABS_MAX 5

if (ax <= LSTICK_DEAD_ABS_MAX && ay <= LSTICK_DEAD_ABS_MAX) {
    return 0;
}
```

目的：

- 减少回中附近微小抖动。
- 与 Arduino 项目行为保持一致。

## 其它 BLE Security 相关修复

为了让 Q34B 能稳定完成配对和后续 notify，本日保留并验证了以下 NimBLE 修改：

### 1. Security 配置对齐 Q34B 当前可用路径

文件：

```text
src/tal_bluetooth/nimble/include/tuya_ble_cfg.h
```

在启用：

```text
ENABLE_QUADDLE_BLE_HID_CENTRAL
```

时设置：

```c
TY_HS_BLE_SM_BONDING = 1
TY_HS_BLE_SM_IO_CAP = BLE_HS_IO_NO_INPUT_OUTPUT
TY_HS_BLE_SM_LEGACY = 1
TY_HS_BLE_SM_OUR_KEY_DIST = 3
TY_HS_BLE_SM_THEIR_KEY_DIST = 3
```

说明：

- 之前尝试开启 Secure Connections `SC=1`，但当前 TuyaOpen NimBLE 构建缺少相关 CMAC 依赖，编译失败，因此回退到 legacy pairing。
- 当前日志已经证明 legacy pairing + bonding 可以让手柄完成连接、震动、蓝灯常亮和 notify。

### 2. 主动发起 GAP security 并记录配置

文件：

```text
src/tal_bluetooth/nimble/tkl_bluetooth.c
```

新增/保留日志：

```text
GAP security cfg sm=1 legacy=1 sc=0 bonding=1 io=3 key=3/3
Start GAP security handle=0x0003 rc=0
```

### 3. 无保存 LTK 时进入 pairing

文件：

```text
src/tal_bluetooth/nimble/host/ble_gap.c
```

逻辑：

- 如果读取不到 stored LTK。
- 或 store backend 不支持读取。
- 则主动进入 pairing。

日志示例：

```text
GAP security: no stored LTK handle=0x0003 read_rc=8, start pair
```

### 4. Store overflow 检查兼容无 store 条件

文件：

```text
src/tal_bluetooth/nimble/host/ble_sm.c
```

当 store backend 返回 unsupported / no entry 时，不再作为致命失败。

日志示例：

```text
SM store overflow check skipped obj_type=2 rc=8
SM store overflow check skipped obj_type=1 rc=8
```

### 5. SM channel 缺失时懒创建

文件：

```text
src/tal_bluetooth/nimble/host/ble_sm_cmd.c
```

修复某些连接时机下 SM channel 尚未创建导致 pairing 命令无法发送的问题。

## 今日主要修改文件

### TuyaOpen 工程

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
apps/tuya.ai/your_chat_bot/src/quaddle_robot_bridge.c
src/tal_bluetooth/nimble/host/ble_att.c
src/tal_bluetooth/nimble/host/ble_gap.c
src/tal_bluetooth/nimble/host/ble_hs_conn.c
src/tal_bluetooth/nimble/host/ble_sm.c
src/tal_bluetooth/nimble/host/ble_sm_cmd.c
src/tal_bluetooth/nimble/include/tuya_ble_cfg.h
src/tal_bluetooth/nimble/tkl_bluetooth.c
```

### Arduino 参考工程

```text
/home/jasonw/Projects/joyDemoS11/QuaddleGame/Bm769GamepadBle.cpp
```

Arduino 侧主要是增加调试输出，不改变核心连接行为。

## 当前验证结果

已执行：

```bash
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
```

编译成功：

```text
====================[ BUILD SUCCESS ]===================
Target    : your_chat_bot_QIO_1.0.1.bin
Output    : /home/jasonw/Projects/TyOpen_Jason/TuyaOpen/apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
Platform  : ESP32
Chip      : esp32s3
Board     : AI_BOARD
Framework : base
========================================================
```

固件中已确认包含新指纹：

```text
quaddle ble hid: central init fix=20260604-poll-filter-v8
```

## 下一步建议

1. 烧录 v8 固件后确认启动指纹：

```text
quaddle ble hid: central init fix=20260604-poll-filter-v8
```

2. 连接 Q34B 后确认 read polling 被跳过：

```text
quaddle ble hid: read polling skipped, readable=2 notify=4
```

3. 摇杆回中后观察：

- 允许继续看到 HID notify，这是手柄持续上报状态，属于正常现象。
- 不应再持续出现假的：

```text
[Q34B] LSTICK -128,128
[Q34B] RSTICK -100,128
[Q34B] DPAD_U+
```

- UART1 不应再持续发送对应动作指令。

4. 如果后续仍有回中动作抖动，优先检查：

- notify 原始帧是否仍为稳定的 `80 7C 80 80 FF 00 00 00 00 8F` 或类似中位帧。
- `[Q34B] LSTICK 0,4` 是否被 bridge 识别为 dead zone / `kup`。
- 是否存在其它非 Q34B 数据源进入 `feed_report()`。
