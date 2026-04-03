/**
 * @file board_com_api.h
 * @brief Common board-level hardware registration API for DNESP32C3_BOX.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __BOARD_COM_API_H__
#define __BOARD_COM_API_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all hardware peripherals (audio codec, button, LCD) on the board.
 *
 * @return OPRT_OK on success, otherwise an error code.
 */
OPERATE_RET board_register_hardware(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_COM_API_H__ */
