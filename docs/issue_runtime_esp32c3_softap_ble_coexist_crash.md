# 问题：ESP32-C3 配网阶段 SoftAP 崩溃（`ieee80211_hostap_attach` / `alloc eb fail`）

## 现象

日志顺序类似：

```text
[ty D][netcfg.c:279] netcfg module start type:0x1
[ty D][ap_netcfg.c:859] ap cfg start:1
I wifi:mode : softAP (...)
W wifi:alloc eb len=752 type=4 fail
W wifi:m f beacon l=224
Guru Meditation Error: Core 0 panic'ed (Load access fault)
--- ieee80211_hostap_attach
```

同时堆余量很低（`largest_block` 约 **2 KB 级**），BLE 广播仍在工作。

## 根因

在 **无 PSRAM**、已启用 **BLE + Station + 大量业务线程** 的前提下，再启动 **SoftAP**，WiFi 固件需要额外分配 **beacon / event buffer**（日志里约 **752 B** 连续块）。堆碎片化或总余量不足时 **`malloc` 失败**，后续 `ieee80211_hostap_attach` 对空指针解引用 → **Load access fault**。

这与 “AP 配网模块已能 `ap_netcfg_init`” 无关——崩溃发生在 **WiFi 驱动层**。

## 修复（PETOI ESP32-C3）

`apps/tuya.ai/petoi_esp32c3_cube/src/tuya_main.c` 将配网方式改为 **仅 BLE**：

```c
netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
```

不再组合 `NETCFG_TUYA_WIFI_AP`。手机 App 使用 **蓝牙配网** 即可；若产品强制要求 AP 配网，需 **更多内部 RAM**（减缓冲 / 关功能）或 **带 PSRAM 的模组**。

## 关联：`datasink_mem` 在绑定点失败

绑定时 `largest_block` 常仅 **~2304**，`AI_PLAYER_RINGBUF_SIZE` 若设为 **3072**，`tuya_ring_buff_create` 易失败。PETOI 工程将 **`CONFIG_AI_PLAYER_RINGBUF_SIZE=2048`**。
