/**
 * @file ai_mcp.h
 * @brief AI MCP (Model Context Protocol) module header
 *
 * This header file defines the functions for initializing and deinitializing
 * the MCP module, which provides tool discovery and execution capabilities.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __AI_MCP_H__
#define __AI_MCP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/


/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef OPERATE_RET (*AI_MCP_VOLUME_CHANGED_CB)(int volume, void *user_data);

/***********************************************************
********************function declaration********************
***********************************************************/
/**
@brief Initialize MCP module
@return OPERATE_RET Operation result
*/
OPERATE_RET ai_mcp_init(void);

/**
@brief Deinitialize MCP module
@return OPERATE_RET Operation result
*/
OPERATE_RET ai_mcp_deinit(void);

/**
@brief Set volume changed callback for app-specific side effects
@param cb Callback invoked after MCP volume set succeeds
@param user_data User data passed to callback
@return OPERATE_RET Operation result
*/
OPERATE_RET ai_mcp_volume_changed_cb_set(AI_MCP_VOLUME_CHANGED_CB cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* __AI_MCP_H__ */
