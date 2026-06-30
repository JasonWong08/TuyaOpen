/**
 * @file ai_mcp_robot_tools.c
 * @brief your_chat_bot-only MCP tool self.robot.send_command (second UART).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Compiled only when CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y (see CMakeLists.txt).
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"

#include "ai_mcp_server.h"

#if !defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) || !ENABLE_CHAT_BOT_ROBOT_SECOND_UART
#error "ai_mcp_robot_tools.c must be built with ENABLE_CHAT_BOT_ROBOT_SECOND_UART=1"
#endif

#include "second_uart.h"
#include "quaddle_robot_bridge.h"

static OPERATE_RET __robot_send_command(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val,
                                        void *user_data)
{
    const char *text       = NULL;
    char normalized_text[64];
    OPERATE_RET mcp_str_rt = OPRT_OK;

    (void)user_data;

    if (!properties || !ret_val) {
        return OPRT_INVALID_PARM;
    }

    for (int i = 0; i < properties->count; i++) {
        MCP_PROPERTY_T *prop = properties->properties[i];
        if (strcmp(prop->name, "text") == 0 && prop->type == MCP_PROPERTY_TYPE_STRING && prop->has_default) {
            text = prop->default_val.str_val;
            break;
        }
    }

    if (!text || strlen(text) == 0) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "empty command");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
    }

    if (strncmp(text, "kbk", 3) == 0 && (text[3] == '\0' || text[3] == ' ' || text[3] == '\t')) {
        snprintf(normalized_text, sizeof(normalized_text), "kbkF%s", text + 3);
        text = normalized_text;
    }

    OPERATE_RET uart_rt = quaddle_robot_bridge_queue_ai_command(text, "MCP");
    if (uart_rt != OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "uart command queue failed");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : uart_rt;
    }

    PR_NOTICE("MCP robot send_command queued \"%s\"", text);

    mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "ok");
    return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
}

OPERATE_RET ai_mcp_robot_tools_register(void)
{
    return AI_MCP_TOOL_ADD(
        "self.robot.send_command",
        "Send physical robot dog motion commands over UART1; this is not for LCD expressions.\n"
        "Arguments must contain command codes only, never natural-language descriptions.\n"
        "Do not mention internal scheduling, queueing, arbitration, or gamepad priority in user-facing replies.\n"
        "Use this tool only when the user explicitly asks for a physical robot body motion. Do not call it for normal "
        "chat, self-introductions, ability descriptions, or question answering. Do not add default gestures or motions. "
        "Always use this tool for explicit robot body motions, including English requests such as sit down, stand up, go "
        "forward, turn left/right, run, crawl, nod, and shake head. Do not route these robot actions to smart_home. "
        "Reply to the user in the same language as the user's latest request. Head motion may use m0 or kwh.\n"
        "Gait: kwkF forward, kbkF backward, ktrF run, kcrF crawl, kvtL turn left, kvtR turn right. "
        "Parameter <=200 means steps/default 3; >200 means milliseconds; turn parameter is usually degrees/default "
        "90.\n"
        "Basic: khi greet, ksit sit, kgdb good, kpu push-up, kjmp jump, d rest, kup stand, kstr stretch.\n"
        "Emotion: kang angry, khg hug, kfiv high-five, kchr cheer, knd nod, kwh shake/tilt head, kzz sleepy.\n"
        "Skills: kbf backflip, kff frontflip, khds handstand, kmw moonwalk, krl roll, kbx boxing, kkc kick.\n"
        "Interaction: kcmh come, khsk handshake, khu raise hand, ksnf sniff, kscrh scratch, kdg dig, kpee pee, kpd "
        "play dead.\n"
        "Head/joints: kwh shake/tilt head, knd nod, m0 angle turns head (positive left, negative right, e.g. m0 45). "
        "m<number> <angle>: head=0, left hand=8, right hand=9; negative is forward, positive is backward.\n"
        "Examples: sit -> ksit; run -> ktrF 3; tilt head -> kwh; turn head left -> m0 45; raise left hand -> m8 -30.",
        __robot_send_command, NULL, MCP_PROP_STR("text", "Machine command code only; no Chinese or descriptions."));
}
