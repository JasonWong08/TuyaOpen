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

#define ROBOT_COMMAND_COMPLETION_TIMEOUT_MS 21000

static void __robot_trim_command(char **start, char **end)
{
    while (*start < *end && (**start == ' ' || **start == '\t' || **start == '\r' || **start == '\n')) {
        (*start)++;
    }
    while (*end > *start &&
           ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r' || (*end)[-1] == '\n')) {
        (*end)--;
    }
}

static bool __robot_normalize_command(char *cmd, size_t cmd_len)
{
    size_t len;

    if (!cmd || cmd_len == 0) {
        return false;
    }
    if (strncmp(cmd, "kbk", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ' || cmd[3] == '\t')) {
        len = strlen(cmd);
        if (len + 1 >= cmd_len) {
            return false;
        }
        memmove(cmd + 4, cmd + 3, len - 2);
        cmd[3] = 'F';
    }
    return true;
}

static OPERATE_RET __robot_send_command(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val,
                                        void *user_data)
{
    const char *text       = NULL;
    char command_text[64];
    OPERATE_RET mcp_str_rt = OPRT_OK;
    OPERATE_RET uart_rt    = OPRT_OK;
    char       *cursor;
    unsigned    queued_count = 0;

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

    if (strlen(text) >= sizeof(command_text)) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "command too long");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_INVALID_PARM;
    }
    snprintf(command_text, sizeof(command_text), "%s", text);

    cursor = command_text;
    while (cursor && *cursor != '\0') {
        char    *segment = cursor;
        char    *end     = strchr(cursor, ';');
        char     cmd[64];
        size_t   cmd_len;
        uint32_t ticket = 0;

        if (end) {
            cursor = end + 1;
        } else {
            end = cursor + strlen(cursor);
            cursor = NULL;
        }

        __robot_trim_command(&segment, &end);
        if (segment == end) {
            continue;
        }

        cmd_len = (size_t)(end - segment);
        if (cmd_len >= sizeof(cmd)) {
            PR_WARN("MCP robot send_command segment too long, drop \"%s\"", text);
            uart_rt = OPRT_INVALID_PARM;
            break;
        }
        memcpy(cmd, segment, cmd_len);
        cmd[cmd_len] = '\0';
        if (!__robot_normalize_command(cmd, sizeof(cmd))) {
            PR_WARN("MCP robot send_command normalize failed, drop \"%s\"", text);
            uart_rt = OPRT_INVALID_PARM;
            break;
        }

        uart_rt = quaddle_robot_bridge_queue_ai_command_tracked(cmd, "MCP", &ticket);
        if (uart_rt != OPRT_OK) {
            break;
        }
        queued_count++;
        PR_NOTICE("MCP robot send_command tracking segment %u \"%s\" ticket=%u", queued_count, cmd, ticket);

        uart_rt = quaddle_robot_bridge_wait_ai_command(ticket, ROBOT_COMMAND_COMPLETION_TIMEOUT_MS);
        if (uart_rt != OPRT_OK) {
            PR_WARN("MCP robot send_command completion failed segment %u \"%s\" ticket=%u rt=%d", queued_count,
                    cmd, ticket, uart_rt);
            break;
        }
        PR_NOTICE("MCP robot send_command completed segment %u \"%s\" ticket=%u", queued_count, cmd, ticket);
    }

    if (queued_count == 0 && uart_rt == OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "empty command");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
    }

    if (uart_rt != OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "robot action not completed; do not claim completion");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : uart_rt;
    }

    mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "completed: robot completion token received");
    return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
}

OPERATE_RET ai_mcp_robot_tools_register(void)
{
    return AI_MCP_TOOL_ADD(
        "self.robot.send_command",
        "Send physical robot dog motion commands over UART1; this is not for LCD expressions.\n"
        "Arguments must contain command codes only, never natural-language descriptions. Prefer one command per tool call; "
        "semicolon-separated commands are accepted only as a fallback.\n"
        "Do not mention internal scheduling, queueing, arbitration, or gamepad priority in user-facing replies.\n"
        "This tool waits for the robot completion token. Before it returns, only say that the action is in progress. "
        "Say the action is complete only when the tool returns 'completed'. If it reports failure or timeout, say that "
        "completion could not be confirmed; never claim the action completed.\n"
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
