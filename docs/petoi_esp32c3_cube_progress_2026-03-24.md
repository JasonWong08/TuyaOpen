# PETOI_ESP32C3_CUBE 今日计划进度总结（2026-03-24）

## 一、计划总览对照（来自 `esp32c3项目收敛计划_d179d4de.plan.md`）

计划中 5 个主任务状态均为 `completed`：

- `baseline-observability`：已完成
- `cloud-first-stabilization`：已完成
- `display-whitescreen-fix`：已完成
- `feature-restore`：已完成
- `release-regression`：已完成

结合今天新增日志与调优结果，当前项目已进入“**连云稳定，但功能恢复受内存门槛限制**”阶段。

---

## 二、今日实际进展（按问题链路）

## 1) 启动稳定性

- 已修复 `tuya_app_main` 栈溢出重启问题（将主线程栈恢复至更稳配置）。
- 设备可稳定启动，无反复 crash/重启风暴。

## 2) 配网与激活链路

- BLE 配网成功，Token/Region/RegistKey 可正常下发。
- iotdns HTTPS 可成功握手（`h6-ay.iot-dns.com:443`）。
- 激活请求成功，日志出现 `TUYA_EVENT_ACTIVATE_SUCCESSED`。

## 3) MQTT 链路

- MQTT TLS 建链成功（`m2.tuyacn.com:8883`）。
- MQTT 连接成功，日志出现 `TUYA_EVENT_MQTT_CONNECTED`。
- 说明“此前卡在 `not authorized(11)`”的主阻塞已在本轮验证中解除。

## 4) 显示与语音现状（当前待收敛）

- 屏幕仍为亮白屏（无有效 UI 内容）。
- 无提示语音。
- 根因并非链路失败，而是 `post-cloud` 初始化被内存阈值拦截：
  - 典型日志：`skip post-cloud AI/UI init, heap=15636 < 20480`

---

## 三、今天做过的关键策略调整

## 1) “先连云再恢复功能”策略收敛

- 取消/延后了会抢占配网阶段内存的离线 UI/音频早启路径，避免影响 iotdns TLS。
- 保持云链路优先，避免再出现 `mbedtls_ssl_setup Fail 0xffff8100`。

## 2) BLE 释放回收内存（已入代码）

- 在首次 `MQTT_CONNECTED` 后，尝试释放 BLE 配网栈：
  - `netcfg_stop(NETCFG_TUYA_BLE)`
  - `tuya_ble_deinit()`
- 目标：回收常驻内存，提高 `post-cloud` 初始化成功率，推动 UI/语音恢复。

## 3) 内存观测结论

- 连云成功后可用堆仍偏低，存在碎片化（`largest_block` 常见约 3KB）。
- `netmgr` 持续打印 `Defer LAN init due low heap`，说明当前已处于保守内存保护模式。

---

## 四、当前结论

今天已完成从“链路不稳定/易失败”到“**配网 + 激活 + MQTT 连通**”的关键收敛，当前主要剩余问题是：

- 在低内存条件下，`post-cloud` 业务初始化未通过，导致：
  - UI 未恢复（白屏）
  - 提示语音未恢复

项目状态可定义为：**核心联网链路已打通，功能层恢复进入最后收口阶段**。

---

## 五、明天烧录验证建议（按顺序）

1. 烧录最新固件后，优先确认是否出现：
   - `BLE released after MQTT connect, heap=...`
2. 观察 `TUYA_EVENT_MQTT_CONNECTED` 后的堆变化：
   - `Device Free heap`
   - `largest_block`
3. 检查是否仍出现：
   - `skip post-cloud AI/UI init, heap=... < 20480`
4. 若该行消失，继续确认：
   - 是否出现 UI 初始化成功日志
   - 屏幕是否恢复内容显示
   - 提示音是否恢复

---

## 六、明日收敛目标（建议）

- 目标 A：保持“配网/激活/MQTT 连通”稳定不回退。
- 目标 B：将 `post-cloud` 以“轻量模式”拉起（先 UI 与本地提示音，再逐步恢复完整 AI）。
- 目标 C：固化最终阈值与开关，形成可发布配置基线。

