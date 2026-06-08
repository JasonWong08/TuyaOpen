# 2026-06-08 Quaddle 手柄优先连接与 BOOT 触发配网恢复记录

## 背景

本次工作基于前期 Quaddle / GamepadSpace-Q34B BLE 手柄与 Tuya BLE 配网共存改造继续推进。

新的目标流程：

- Tuya 主板上电稳定后，用户按手柄 Home 键给手柄上电。
- 手柄优先与主板建立 BLE HID 连接。
- 手柄连接成功后有震动反馈，手柄蓝牙指示灯常亮。
- 手柄可以直接控制机器狗动作。
- 需要 Tuya 配网时，短按主板 BOOT 键。
- 如果手柄已连接，则先临时断开手柄 BLE。
- 主板进入 Tuya 手机 App BLE 配网流程。
- 配网成功并联网后，自动恢复手柄 BLE 连接和控制能力。

相关工程路径：

```text
apps/tuya.ai/your_chat_bot
```

参考日志：

```text
/home/jasonw/Documents/手柄蓝牙调试log17.txt
/home/jasonw/Documents/手柄蓝牙调试log18.txt
```

## 今日主要进展

### 1. 将上电默认流程改为手柄优先

原来的行为：

- 主板上电后会立即注册并启动 Tuya BLE 配网。
- 手机 App 可以扫描到设备并配网。
- 但这会让 Tuya BLE 配网链路优先占用 BLE 时机，不符合“先连接手柄，按 BOOT 再配网”的新流程。

修改后：

- 上电时只准备 Tuya BLE 配网能力，不立即启动配网。
- 手柄 BLE central 可以先扫描并连接保存过的 GamepadSpace-Q34B 手柄。
- 用户短按 BOOT 后才显式启动 Tuya BLE 配网。

涉及文件：

```text
apps/tuya.ai/your_chat_bot/src/tuya_main.c
src/tuya_cloud_service/netmgr/netmgr.h
src/tuya_cloud_service/netmgr/netconn_wifi.h
src/tuya_cloud_service/netmgr/netconn_wifi.c
```

核心修改：

- 新增 `NETCONN_CMD_NETCFG_PREPARE`。
- `NETCONN_CMD_NETCFG_PREPARE` 只注册 token getter，不立即启动 `netcfg_start()`。
- 保留原 `NETCONN_CMD_NETCFG` 的立即启动行为，供 BOOT 短按触发时使用。

关键代码行为：

```c
netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG_PREPARE, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
```

### 2. 新增 BOOT 短按进入 Tuya BLE 配网

原来的 BOOT 逻辑：

- 手柄模块轮询 GPIO0。
- BOOT 长按 2 秒用于清除保存的手柄配对信息。

本次扩展：

- BOOT 短按用于进入 Tuya BLE 配网。
- BOOT 长按 2 秒仍保留为清除手柄配对。
- 短按和长按互不冲突。

涉及文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

新增行为：

1. 检测 BOOT 短按释放。
2. 打印日志：

   ```text
   quaddle ble hid: BOOT short press: pause gamepad and enter Tuya BLE provisioning
   ```

3. 调用 `set_wifi_busy_internal(true)`。
4. 如果手柄正在扫描、连接或已连接，则停止扫描、取消连接或断开手柄。
5. 延迟 500 ms 后启动 Tuya BLE 配网：

   ```text
   quaddle ble hid: start Tuya BLE provisioning
   ```

6. 调用：

   ```c
   netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
   ```

### 3. 配网期间临时让出 BLE 和 Wi-Fi 共存窗口

沿用前期共存机制：

- Tuya BLE 配网收到 Wi-Fi 参数后发布 `netcfg.wifi`。
- 手柄 BLE central 进入 `wifi_busy`。
- `TUYA_EVENT_MQTT_CONNECTED` 后恢复为 `wifi coexist normal`。

配网期间手柄模块会：

- 停止手柄扫描。
- 停止重连定时器。
- 停止 HID read polling。
- 取消正在进行的手柄连接。
- 断开已经连接的手柄。

关键日志：

```text
quaddle ble hid: wifi coexist busy
quaddle ble hid: pause gamepad BLE during WiFi provisioning
quaddle ble hid: wifi coexist normal
```

## log17 验证结论

`log17` 对应的测试结果：

- 先等待 Tuya 主板上电稳定并听到可配网提示音。
- 先进行 Tuya BLE 配网，配网可以成功。
- 配网后可以通过 AI 对话控制机器狗动作。
- 再连接手柄，手柄 BLE 可以连接成功。
- 手柄可以控制机器狗动作。

该日志证明：

- Tuya BLE 配网链路本身可用。
- 手柄 BLE HID 链路本身可用。
- AI 控制机器狗链路本身可用。

因此本次主要调整的是启动顺序和状态切换方式。

## log18 问题定位

上传第一版新固件后的测试现象：

1. 上电后先按手柄 Home。
2. 手柄连接成功，蓝牙灯常亮，有震动反馈。
3. 手柄可以控制机器狗。
4. 短按 BOOT 后，手机 App 可以扫描到 Tuya 设备并进入配网。
5. 配网期间手柄蓝牙灯开始闪烁，说明手柄连接被临时断开，符合预期。
6. 配网成功后可以进行 AI 对话。
7. 手柄蓝牙灯又变为常亮，看起来像是恢复连接。
8. 但操控手柄后没有串口输入日志，机器狗也无动作。

关键日志片段：

```text
quaddle ble hid: target adv name=GamepadSpace-Q34B
quaddle ble hid: connecting name=GamepadSpace-Q34B
quaddle ble hid: connected handle=3 name=GamepadSpace-Q34B
ble monitor check iot is connected, stop adv!
ble_gap_terminate:
quaddle ble hid: disconnected reason=534
```

结论：

- 配网成功后，手柄确实重新建立了 GAP 连接。
- 但在 HID 服务发现和 input report 订阅完成前，Tuya BLE 管理器检测到 IoT 已连接，执行停止广播逻辑。
- 该逻辑触发底层 `ble_gap_terminate`，把刚恢复的手柄连接断开。
- 因此没有出现：

  ```text
  quaddle ble hid: ready, subscribed ...
  ```

- 手柄灯常亮只说明链路层曾经连接过，不代表 HID 输入报告已经可用。

## 第二轮修复

### 1. 配网成功后延迟恢复手柄扫描

为避开 Tuya BLE manager 在云连接后停止广播、清理旧连接的窗口，本次将 `wifi_busy -> normal` 后的手柄扫描恢复延迟 4 秒。

涉及文件：

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
```

新增宏：

```c
#define QUADDLE_BLE_WIFI_RESUME_SCAN_DELAY_MS 4000
```

行为：

- 普通断开仍按原有 1 秒 rescan。
- 从 Wi-Fi 配网 busy 恢复时，延迟 4 秒再扫描手柄。
- 避免手柄刚连上就被 Tuya BLE monitor 的 stop adv 流程误断开。

预期日志顺序：

```text
quaddle ble hid: wifi coexist normal
... wait about 4s ...
quaddle ble hid: target adv name=GamepadSpace-Q34B
quaddle ble hid: connected handle=...
quaddle ble hid: ready, subscribed ...
```

### 2. 设备已经在线后忽略 BOOT 短按

`log18` 后半段中，设备已经配网成功且 MQTT 在线后，又出现多次 BOOT 短按触发：

```text
quaddle ble hid: BOOT short press: pause gamepad and enter Tuya BLE provisioning
quaddle ble hid: wifi coexist busy
```

这会让手柄恢复再次被 `wifi_busy` 阻塞，直到 45 秒超时。

本次增加保护：

- 如果 `tuya_iot_is_connected()` 返回 true，说明设备已经在线。
- 此时 BOOT 短按不再进入配网流程。
- 打印：

  ```text
  quaddle ble hid: BOOT short press ignored, device already online
  ```

目的：

- 防止在线状态下误触 BOOT 造成手柄再次被暂停。
- 保留 BOOT 长按 2 秒清除手柄配对的维护能力。

## 最终验证结果

用户反馈第二版固件验证正常：

- 主板上电稳定后，按手柄 Home 可以连接手柄。
- 手柄连接成功后有震动反馈，蓝牙指示灯常亮。
- 手柄可控制机器狗动作。
- 短按 BOOT 后，手柄连接会临时断开。
- 手机 App 可以扫描 Tuya 设备并完成正常配网流程。
- 配网成功后 AI 对话可用。
- 手柄 BLE 能自动恢复。
- 恢复后手柄控制机器狗正常。

## 构建与检查

已执行：

```bash
. ./export.sh
cd apps/tuya.ai/your_chat_bot
tos.py build
```

结果：

```text
BUILD SUCCESS
Target: your_chat_bot_QIO_1.0.1.bin
Output: apps/tuya.ai/your_chat_bot/dist/your_chat_bot_1.0.1
Platform: ESP32
Chip: esp32s3
Board: AI_BOARD
Framework: base
```

已执行：

```bash
git diff --check
```

结果：

```text
通过，无空白错误
```

格式脚本情况：

```bash
python3 tools/check_format.py --debug --files ...
```

当前环境缺少 `clang-format`，因此格式脚本无法完整运行：

```text
Error: clang-format is not installed or not in PATH
```

## 修改文件汇总

```text
apps/tuya.ai/your_chat_bot/src/quaddle_ble_hid_central.c
apps/tuya.ai/your_chat_bot/src/tuya_main.c
src/tuya_cloud_service/netmgr/netconn_wifi.c
src/tuya_cloud_service/netmgr/netconn_wifi.h
src/tuya_cloud_service/netmgr/netmgr.h
```

## 关键行为总结

最终流程：

1. 上电后不自动启动 Tuya BLE 配网。
2. 手柄可优先连接主板。
3. BOOT 短按触发 Tuya BLE 配网。
4. 配网期间手柄 BLE 临时断开。
5. MQTT 在线后恢复 `wifi coexist normal`。
6. 延迟 4 秒后恢复扫描并连接手柄。
7. 等到 `ready, subscribed ...` 出现后，手柄 HID 输入恢复，机器狗可再次被手柄控制。
8. 在线状态下 BOOT 短按被忽略，避免误触导致手柄再次进入 busy 暂停。
9. BOOT 长按 2 秒仍用于清除手柄配对。

