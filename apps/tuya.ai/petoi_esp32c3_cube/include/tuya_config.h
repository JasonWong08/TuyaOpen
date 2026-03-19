/**
 * @file tuya_config.h
 * @brief IoT device credentials for petoi_esp32c3_cube.
 *
 * TUYA_PRODUCT_ID  — set via CONFIG_TUYA_PRODUCT_ID in app_default.config;
 *                    the #ifndef guard below acts as a last-resort fallback only.
 * TUYA_OPENSDK_UUID / AUTHKEY — replace with values from the Tuya IoT platform:
 *   https://developer.tuya.com/cn/docs/iot-device-dev/application-creation?id=Kbxw7ket3aujc
 */

#ifndef TUYA_CONFIG_H_
#define TUYA_CONFIG_H_

#include "tuya_cloud_types.h"

#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID "xlq6xkhdgvx0d0rl"
#endif

// #define TUYA_OPENSDK_UUID    "uuidxxxxxxxxxxxxxxxx"             /* TODO: replace */
// #define TUYA_OPENSDK_AUTHKEY "keyxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" /* TODO: replace */
#define TUYA_OPENSDK_UUID    "uuid683dda876825d1b7"              /* TODO: replace */
#define TUYA_OPENSDK_AUTHKEY "G3hsD0x6d5xWqAMWKSarbv90RvgmsbZm"  /* TODO: replace */

// #define TUYA_NETCFG_PINCODE   "69832860"

#endif
