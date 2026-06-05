# 2026-06-05 Quaddle 手柄 BLE 与 Tuya App 配网共存修复记录

## 背景

本次继续基于前几天的 Quaddle / GamepadSpace-Q34B BLE 手柄移植工作推进。当前目标是：

- 手柄通过 BLE 连接主板后，可以稳定控制机器狗。
- Tuya 手机 App 可以通过 BLE 对主板配网。
- 两个功能互不干扰。
- 串口日志保持可读，避免高频无效日志干扰判断。

相关工程路径：

```text
apps/tuya.ai/your_chat_bot
```

参考日志：

```text
/home/jasonw/Documents/手柄蓝牙调试log12.txt
/home/jasonw/Documents/手柄蓝牙调试log13.txt
/home/jasonw/Documents/手柄蓝牙调试log14.txt
/home/jasonw/Documents/手柄蓝牙调试log15.txt
```

## 今日主要进展

### 1. 确认并修复 BLE 多连接/多回调冲突

早期现象：

- 手柄 BLE 已连接，机器狗可被手柄控制。
- Tuya App 配网时发生异常、重启或无法配网。
- 手机 App BLE 配网连接和手柄 BLE central 连接同时存在时，底层事件有互相干扰。

根因：

- 原来 BLE 连接数不足，默认更接近单连接模型。
- TKL/TAL 层 GAP/GATT callback 之前按单 callback 思路使用。
- Tuya 手机 App 是 peripheral 方向连接主板；手柄是主板作为 central 连接外设。两个角色同时存在时，事件需要按角色和连接 handle 隔离。

修改：

- `src/tal_bluetooth/CMakeLists.txt`
  - 增加 `TY_HS_BLE_MAX_CONNECTIONS=2`。
- `src/tal_bluetooth/nimble/host/ble_gap.c`
  - 给 `ble_hs_conn_alloc()` 结果增加空指针保护，避免连接数不足时直接异常。
- `src/tal_bluetooth/nimble/tkl_bluetooth.c`
  - 支持多个 GAP/GATT callback 注册和分发。
  - BLE stack 已初始化时，也正确设置角色状态。
- `src/tal_bluetooth/src/tal_bluetooth.c`
  - 增加 TAL 层运行时角色过滤。
  - Tuya TAL 只处理手机 App peripheral 连接事件。
  - Quaddle 手柄模块只处理 central 方向的手柄事件。

### 2. 精简 BLE HID 高频日志

用户反馈日志中高频出现：

```text
[tkl_bluetooth.c:219] receive notify ok, handle=0x001b len=10
[quaddle_ble_hid_central.c:1161] quaddle ble hid: report handle=27 len=10
```

处理：

- 移除或降级 HID notify 高频日志。
- 保留关键状态日志，例如连接、断开、ready、配网 busy/normal。
- 对手柄连接过程中的服务、特征、描述符发现日志使用更低等级，默认日志等级下只看关键事件。

### 3. 分析 log13：设备重启不是崩溃，而是配网失败后 Tuya reset

log13 中没有 Guru Meditation / panic。

实际流程：

1. Tuya App 通过 BLE 成功下发 WiFi 信息。
2. 固件打印：

   ```text
   cfg ssid:CAT-2.4G, passwd:qwertyuiop, token:...
   ```

3. WiFi 连接持续失败：

   ```text
   WIFI_EVENT_STA_DISCONNECTED Disconnect reason : 201
   ```

4. `201` 对应 `WIFI_REASON_NO_AP_FOUND`。
5. 超时后 Tuya App 发 reset 请求，进入 `TUYA_EVENT_RESET`，因此发生重启。

结论：

- 这次重启不是代码 panic。
- 核心问题是手柄 BLE 通信和 WiFi 配网连接共存时，WiFi 扫描/连接不稳定。

### 4. 增加手柄 BLE / WiFi 配网共存模式

修改文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
apps/tuya.ai/your_chat_bot/include/quaddle_ble_hid_central.h
src/tuya_cloud_service/ble/ble_netcfg.c
apps/tuya.ai/your_chat_bot/src/tuya_main.c
```

新增机制：

- 收到 Tuya BLE 配网 WiFi token 后，发布事件：

  ```text
  netcfg.wifi
  ```

- Quaddle 手柄 BLE central 订阅该事件。
- 进入 WiFi 配网 busy 状态时：
  - 停止手柄 BLE 扫描。
  - 停止手柄重连定时器。
  - 停止 HID read polling。
  - 若正在连接手柄，则取消连接。
  - 若手柄已连接，则临时断开手柄，给 WiFi 连接一个干净窗口。
- MQTT 连接成功后恢复 normal 状态并重新扫描/连接手柄。
- 如果配网长时间未成功，45 秒后自动恢复手柄 BLE，避免机器狗长期不能操作。

关键日志：

```text
quaddle ble hid: wifi coexist busy
quaddle ble hid: pause gamepad BLE during WiFi provisioning
quaddle ble hid: wifi coexist normal
quaddle ble hid: wifi busy timeout, resume gamepad BLE
```

### 5. log14 中继续定位：同时启动 BLE 配网和 AP 配网导致 WiFi 状态更乱

log14 中出现：

```text
esp_wifi_connect failed(ret=0x300a)
WIFI_EVENT_STA_DISCONNECTED Disconnect reason : 201
```

当时流程中同时存在：

- Tuya BLE 配网。
- Tuya WiFi AP 配网。
- 手柄 BLE central。
- WiFi 从 AP/配网状态切换到 STA 连接状态。

处理：

- `apps/tuya.ai/your_chat_bot/src/tuya_main.c`
  - 将网络配置方式改为只启用 `NETCFG_TUYA_BLE`。
  - 不再同时启动 `NETCFG_TUYA_WIFI_AP`。

修改前：

```c
NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP
```

修改后：

```c
NETCFG_TUYA_BLE
```

目的：

- 当前主要测试路径是 Tuya 手机 App 通过 BLE 配网。
- 去掉 SoftAP 配网线程和 WiFi AP/STA 模式切换，减少与手柄 BLE central 的冲突面。

### 6. log15 中定位：Tuya BLE 管理器误启 central 角色，抢手柄 GATT discovery

上传新固件后，先连接手柄即出现：

```text
quaddle ble hid: no HID/vendor input characteristic found
quaddle ble hid: service discovery failed -28686
quaddle ble hid: disconnected reason=534
```

并且反复连接、反复失败。

根因：

- `src/tuya_cloud_service/ble/ble_mgr.c` 中 Tuya BLE 管理器初始化为：

  ```c
  TAL_BLE_ROLE_PERIPERAL | TAL_BLE_ROLE_CENTRAL
  ```

- 但当前 Tuya BLE 配网只需要主板作为 BLE peripheral 被手机 App 连接。
- TAL 层看到 central 角色后，会在手柄连接成功时自动启动一套 Tuya GATT service discovery。
- Quaddle 手柄模块自己也会对同一条连接启动 HID GATT discovery。
- 两套 discovery 抢同一连接，导致：
  - 服务发现失败 `-28686`。
  - 或服务列表/特征列表被另一套 discovery 清空。
  - 最终找不到 HID/vendor input characteristic。

修复：

```text
src/tuya_cloud_service/ble/ble_mgr.c
```

将 Tuya BLE 管理器角色限制为 peripheral：

```c
ble->role = TAL_BLE_ROLE_PERIPERAL;
```

效果：

- Tuya App 仍可通过 BLE 连接主板配网。
- 手柄 central 连接和 HID GATT discovery 只由 `quaddle_ble_hid_central` 管理。
- 避免 TAL 层自动对手柄执行 Tuya 服务发现。

### 7. 串口指令日志换行显示修复

用户发现日志：

```text
second_uart: TX UART1 GPIO17/18 5 bytes: "kwkF
"
```

分析：

- 这不是单纯日志问题。
- `second_uart_send_string()` 会在命令末尾主动追加 `\n`。
- 实际发送长度 `5 bytes` 中包含 `k w k F \n`。
- 日志之前用 `%.*s` 原样打印缓冲区，所以末尾换行会把第二个双引号挤到下一行。

处理：

```text
apps/tuya.ai/your_chat_bot/src/second_uart.c
```

- 将日志输出改为转义显示。
- `\n` 显示为 `\\n`。
- `\r` 显示为 `\\r`。
- 非可打印字节显示为 `\xNN`。

新日志预期：

```text
second_uart: TX UART1 GPIO17/18 5 bytes: "kwkF\n"
```

注意：

- 这次只修改日志显示。
- 实际 UART1 发送行为不变，仍发送命令末尾换行。

## 今日修改文件汇总

```text
apps/tuya.ai/your_chat_bot/include/quaddle_ble_hid_central.h
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
apps/tuya.ai/your_chat_bot/src/second_uart.c
apps/tuya.ai/your_chat_bot/src/tuya_main.c
src/tal_bluetooth/CMakeLists.txt
src/tal_bluetooth/nimble/host/ble_gap.c
src/tal_bluetooth/nimble/tkl_bluetooth.c
src/tal_bluetooth/src/tal_bluetooth.c
src/tuya_cloud_service/ble/ble_mgr.c
src/tuya_cloud_service/ble/ble_netcfg.c
```

## 验证记录

今日多次执行构建：

```bash
. ./export.sh && cd apps/tuya.ai/your_chat_bot && tos.py build
```

最终构建成功。

固件输出目录：

```text
apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
```

构建中仍有 ESP-SR 相关链接警告：

```text
closedir/opendir/readdir is not implemented and will always fail
```

该警告为既有平台/组件警告，本次未处理。

## 当前预期行为

### 先连接手柄

预期：

- 不再反复出现：

  ```text
  service discovery failed -28686
  no HID/vendor input characteristic found
  ```

- 手柄连接后应进入：

  ```text
  quaddle ble hid: ready, subscribed ... input report(s)
  ```

- 摇杆/按键可以继续通过 UART1 控制机器狗。

### Tuya App BLE 配网

预期：

- App BLE 连接主板后可以下发 WiFi token。
- 进入 WiFi 连接阶段时，手柄 BLE central 临时暂停/断开。
- WiFi/MQTT 成功后，手柄 BLE 自动恢复扫描和连接。
- 若配网未成功，45 秒后手柄 BLE 自动恢复，避免机器狗长期不可控。

### UART1 指令日志

预期：

```text
second_uart: TX UART1 GPIO17/18 5 bytes: "kwkF\n"
```

即日志显示不再跨行，实际 UART 发送仍包含换行。

## 后续建议

- 用最新固件重新测试 `log15` 场景：只开机后先连接手柄，不打开 Tuya App。
- 重点观察是否进入 `ready, subscribed ...`。
- 再测试 Tuya App BLE 配网，观察 WiFi 是否还持续 `reason 201`。
- 如果 WiFi 仍失败，需要进一步区分：
  - AP 是否真实可见。
  - 国家码/信道是否匹配。
  - 密码是否正确。
  - WiFi STA connect 前是否仍有其它模块抢占 WiFi/BLE 共存状态。
