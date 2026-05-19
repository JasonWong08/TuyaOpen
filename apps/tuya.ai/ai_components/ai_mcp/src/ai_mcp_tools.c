/**
 * @file ai_mcp_tools.c
 * @brief MCP tools for exposing local AI device capabilities.
 * @version 0.1
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include <string.h>

#include "tal_api.h"

#include "tuya_ai_agent.h"

#include "ai_manage_mode.h"

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
#include "ai_chat_main.h"
#endif

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
#include "ai_video_input.h"
#endif

#include "ai_mcp_server.h"

#include "ai_mcp.h"
/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/

/***********************************************************
***********************function define**********************
***********************************************************/
static OPERATE_RET __get_device_info(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val,
                                     void *user_data)
{
    cJSON *json = NULL;

    json = cJSON_CreateObject();
    if (!json) {
        PR_ERR("Create JSON object failed");
        return OPRT_MALLOC_FAILED;
    }

    // Implement device info retrieval logic here
    // Add device information
    cJSON_AddStringToObject(json, "model", PROJECT_NAME);
    cJSON_AddStringToObject(json, "serialNumber", "123456789");
    cJSON_AddStringToObject(json, "firmwareVersion", PROJECT_VERSION);

    // Set return value
    ai_mcp_return_value_set_json(ret_val, json);

    return OPRT_OK;
}
#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
static OPERATE_RET __take_photo(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    OPERATE_RET rt         = OPRT_OK;
    uint8_t    *image_data = NULL;
    uint32_t    image_size = 0;

    TUYA_CALL_ERR_LOG(ai_video_display_start());

    tal_system_sleep(3000);

    rt = ai_video_get_jpeg_frame(&image_data, &image_size);
    if (OPRT_OK != rt) {
        PR_ERR("get jpeg frame err, rt:%d", rt);
        return rt;
    }

    rt = ai_mcp_return_value_set_image(ret_val, MCP_IMAGE_MIME_TYPE_JPEG, image_data, image_size);
    if (OPRT_OK != rt) {
        PR_ERR("set return image err, rt:%d", rt);
        ai_video_jpeg_image_free(&image_data);
        return rt;
    }

    ai_video_jpeg_image_free(&image_data);

    TUYA_CALL_ERR_LOG(ai_video_display_stop());

    return OPRT_OK;
}
#endif

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
static OPERATE_RET __get_volume(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    (void)properties;
    (void)user_data;

    int volume = ai_chat_get_volume();

    PR_DEBUG("get volume %d", volume);

    ai_mcp_return_value_set_int(ret_val, volume);

    return OPRT_OK;
}

static OPERATE_RET __set_volume(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    OPERATE_RET rt     = OPRT_OK;
    uint32_t    volume = 50; // default volume

    PR_DEBUG("__set_volume enter");

    // Parse properties to get volume
    for (int i = 0; i < properties->count; i++) {
        MCP_PROPERTY_T *prop = properties->properties[i];
        if (strcmp(prop->name, "volume") == 0 && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
            volume = prop->default_val.int_val;
            break;
        }
    }

    rt = ai_chat_set_volume(volume);
    PR_DEBUG("set volume to %d rt:%d", volume, rt);

    // Set return value
    ai_mcp_return_value_set_bool(ret_val, (rt == OPRT_OK) ? TRUE : FALSE);

    PR_DEBUG("__set_volume exit");

    return OPRT_OK;
}
#endif
static OPERATE_RET __set_mode(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    AI_CHAT_MODE_E mode = 0;

    ai_mode_get_curr_mode(&mode);

    // Parse properties to get volume
    for (int i = 0; i < properties->count; i++) {
        MCP_PROPERTY_T *prop = properties->properties[i];
        if (strcmp(prop->name, "mode") == 0 && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
            mode = prop->default_val.int_val;
            break;
        }
    }

    // Implement actual volume setting logic here
    OPERATE_RET rt = ai_mode_switch(mode);

    PR_DEBUG("set mode to %d rt:%d", mode, rt);

    // Set return value
    ai_mcp_return_value_set_bool(ret_val, (rt == OPRT_OK) ? TRUE : FALSE);

    return OPRT_OK;
}

/**
 * Optional app-specific MCP tools (e.g. your_chat_bot robot UART).
 * Weak default is no-op; your_chat_bot provides a strong override in ai_mcp_robot_tools.c.
 */
__attribute__((weak)) OPERATE_RET ai_mcp_app_tools_register(void)
{
    return OPRT_OK;
}

static OPERATE_RET __ai_mcp_tools_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    // device info get tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD("device_info_get",
                                       "Get device information such as model, serial number, and firmware version.",
                                       __get_device_info, NULL),
                       err);

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
    // device camera take photo tool
    TUYA_CALL_ERR_GOTO(
        AI_MCP_TOOL_ADD("device_camera_take_photo",
                        "Captures one or more photos using the device camera. Use when picture or scene "
                        "change is detected, for visitor detection, or when the user asks for a photo.\n"
                        "Parameters:\n"
                        "- count (int): Number of photos to capture (1-10).\n"
                        "Returns: Captured image(s) in Base64 format.",
                        __take_photo, NULL, MCP_PROP_STR("question", "The question prompting the photo capture."),
                        MCP_PROP_INT_DEF_RANGE("count", "Number of photos to capture (1-10).", 1, 1, 10)),
        err);
#endif

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    // get volume tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD("device_audio_volume_get",
                                       "Gets the device's current volume level.\n"
                                       "Parameters:\n"
                                       "- None.\n"
                                       "Response:\n"
                                       "- Returns the current volume level (0-100).",
                                       __get_volume, NULL),
                       err);

    // set volume tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD("device_audio_volume_set",
                                       "Sets the device's volume level.\n"
                                       "Parameters:\n"
                                       "- volume (int): The volume level to set (0-100).\n"
                                       "Response:\n"
                                       "- Returns true if the volume was set successfully.",
                                       __set_volume, NULL,
                                       MCP_PROP_INT_RANGE("volume", "The volume level to set (0-100).", 0, 100)),
                       err);
#endif

    TUYA_CALL_ERR_GOTO(
        AI_MCP_TOOL_ADD("device_audio_mode_set",
                        "Sets the device's chat mode.\n"
                        "Parameters:\n"
                        "- mode (integer): The chat mode (0=hold, 1=key_press, 2=wakeup, 3=free).\n"
                        "Response:\n"
                        "- Returns true if the mode was set successfully.",
                        __set_mode, NULL,
                        MCP_PROP_INT_RANGE("mode", "The chat mode (0=hold, 1=key_press, 2=wakeup, 3=free)", 0, 3)),
        err);

    TUYA_CALL_ERR_GOTO(ai_mcp_app_tools_register(), err);

    return OPRT_OK;

err:
    // destroy MCP server on failure
    ai_mcp_server_destroy();

    return rt;
}

static OPERATE_RET __ai_mcp_init(void *data)
{
    OPERATE_RET rt = OPRT_OK;

    // FIXME: Set actual MCP server name and mcp version
    TUYA_CALL_ERR_RETURN(ai_mcp_server_init("Tuya MCP Server", "1.0"));

    TUYA_CALL_ERR_RETURN(__ai_mcp_tools_register());

    PR_DEBUG("MCP Server initialized successfully");

    return rt;
}

OPERATE_RET ai_mcp_init(void)
{
    return tal_event_subscribe(EVENT_MQTT_CONNECTED, "ai_mcp_init", __ai_mcp_init, SUBSCRIBE_TYPE_ONETIME);
}

OPERATE_RET ai_mcp_deinit(void)
{
    ai_mcp_server_destroy();

    PR_DEBUG("MCP Server deinitialized successfully");

    return OPRT_OK;
}