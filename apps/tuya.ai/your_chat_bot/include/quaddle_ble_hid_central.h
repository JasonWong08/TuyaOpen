/**
 * @file quaddle_ble_hid_central.h
 * @brief BLE HID central for BM769/Q34B gamepads.
 */
#ifndef YOUR_CHAT_BOT_QUADDLE_BLE_HID_CENTRAL_H
#define YOUR_CHAT_BOT_QUADDLE_BLE_HID_CENTRAL_H

#include <stdbool.h>

#include "tuya_cloud_types.h"

#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET quaddle_ble_hid_central_init(void);
OPERATE_RET quaddle_ble_hid_central_clear_saved(void);
void quaddle_ble_hid_central_set_wifi_busy(bool busy);

#ifdef __cplusplus
}
#endif

#endif /* ENABLE_QUADDLE_BLE_HID_CENTRAL */

#endif /* YOUR_CHAT_BOT_QUADDLE_BLE_HID_CENTRAL_H */
