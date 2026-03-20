# 问题：BLE 配网时 `BLE_INIT: Malloc failed` / `ble packet len err`

## 现象

- 绑定阶段提示音可播放，但手机连上 BLE 后出现：
  - `E BLE_INIT: Malloc failed`
  - `ble packet len err:53686`（长度异常，多为堆损坏或未初始化缓冲）
  - `hci_unknown hci event 26`、随后断开（如 `0x213`）
- 同时 `tuya_main` 周期性打印 `min_ever=12`、`largest_block` 仅几百字节。

## 根因

无 PSRAM 的 ESP32-C3 在 **WiFi + BLE + LVGL + 音频** 同时工作时，**堆余量过低**；BLE 主机/控制器路径上 **`malloc` 失败** → 协议解析拿到坏长度 → 断连。

## 已做缓解（PETOI / sdkconfig）

1. **WiFi 动态缓冲**（`sdkconfig_esp32c3_16m`）：`DYNAMIC_RX/TX` **16→10**，`MGMT_SBUF` **32→20**，为 BLE 让出内部 RAM。
2. **蓝牙控制器流控/去重缓存**：`ADV_REPORT_FLOW_CTRL_NUM` **100→40**，`SCAN_DUPL_CACHE_SIZE` **100→40**（略降扫描侧占用，需全量重新生成 `sdkconfig` 后生效）。
3. **显示**：`DISPLAY_LVGL_FULL_REFRESH` + 略减小 partial 缓冲，并设 **`DISPLAY_ST7789_Y_GAP`** 改善顶部雪花（见 `board_config.h`）。

## 验证

修改 `sdkconfig_esp32c3_16m` 后建议在 `platform/ESP32/tuya_open_sdk` 执行 **`idf.py fullclean`**（或删构建目录）再编译，避免旧 `sdkconfig` 覆盖新默认值。

若仍 `Malloc failed`，可继续小幅下调 WiFi `DYNAMIC_*`，或把 **`DISPLAY_ST7789_Y_GAP`** 改为 **0 / 52** 试不同模组规格。
