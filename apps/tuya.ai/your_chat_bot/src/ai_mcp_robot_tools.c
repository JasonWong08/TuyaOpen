/**
 * @file ai_mcp_robot_tools.c
 * @brief your_chat_bot-only MCP tool self_robot_send_command (second UART).
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
#include "robot_uart_voice.h"

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
    unsigned    skipped_head_count = 0;
    bool        eye_color_request = false;
    bool        mouse_transform_request = false;

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
    eye_color_request = robot_uart_voice_get_pending_eye_color_command(command_text, sizeof(command_text));
    if (eye_color_request) {
        PR_NOTICE("MCP robot send_command: replace \"%s\" with pending eye-color command \"%s\"", text,
                  command_text);
    } else {
        snprintf(command_text, sizeof(command_text), "%s", text);
    }

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

        if (!robot_uart_voice_allow_mcp_head_gesture(cmd)) {
            skipped_head_count++;
            PR_NOTICE("MCP robot send_command skip conversational \"%s\"; firmware owns yes/no", cmd);
            continue;
        }

        uart_rt = quaddle_robot_bridge_queue_ai_command_tracked(cmd, "MCP", &ticket);
        if (uart_rt != OPRT_OK) {
            break;
        }
        queued_count++;
        PR_NOTICE("MCP robot send_command tracking segment %u \"%s\" ticket=%u", queued_count, cmd, ticket);

        if (strcmp(cmd, "XO") == 0 || strcmp(cmd, "Xo") == 0) {
            mouse_transform_request = true;
            continue;
        }

        uart_rt = quaddle_robot_bridge_wait_ai_command(ticket, ROBOT_COMMAND_COMPLETION_TIMEOUT_MS);
        if (uart_rt != OPRT_OK) {
            PR_WARN("MCP robot send_command completion failed segment %u \"%s\" ticket=%u rt=%d", queued_count,
                    cmd, ticket, uart_rt);
            break;
        }
        PR_NOTICE("MCP robot send_command completed segment %u \"%s\" ticket=%u", queued_count, cmd, ticket);
    }

    if (queued_count == 0 && uart_rt == OPRT_OK) {
        if (skipped_head_count > 0) {
            mcp_str_rt = ai_mcp_return_value_set_str(
                ret_val,
                "skipped: firmware nods (knd) or shakes (kwh) from yes/no meaning such as 没错/是的/不行. "
                "Call knd/kwh only when the user explicitly asks to nod or shake. Do not claim you already nodded or shook.");
            return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
        }
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "empty command");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
    }

    if (uart_rt != OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "robot action not completed; do not claim completion");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : uart_rt;
    }

    if (eye_color_request) {
        robot_uart_voice_mark_eye_color_mcp_completed();
        mcp_str_rt = ai_mcp_return_value_set_str(
            ret_val,
            "completed: the robot's eye color was changed. In the user-facing reply, speak as the robot in first "
            "person. In Chinese say '我的眼睛' or '我的眼睛颜色'; never say '你的眼睛' or '眼部灯光'.");
    } else if (mouse_transform_request) {
        mcp_str_rt = ai_mcp_return_value_set_str(
            ret_val,
            "accepted: speak 'as you wish' or '如你所愿' now in this turn. Firmware sends XO/Xo after that speech. "
            "Do not wait for a robot token and do not delay the spoken line.");
    } else {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "completed: robot completion token received");
    }
    return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
}

OPERATE_RET ai_mcp_robot_tools_register(void)
{
    return AI_MCP_TOOL_ADD(
        "self_robot_send_command",
        "Send robot dog body-motion or eye-color commands over UART1; this is not for LCD expressions.\n"
        "Arguments must contain command codes only, never natural-language descriptions. Prefer one command per tool call; "
        "semicolon-separated commands are accepted only as a fallback.\n"
        "Do not mention internal scheduling, queueing, arbitration, or gamepad priority in user-facing replies.\n"
        "Speak as the robot in first person. Refer to the robot's own body and features as my/mine, never your/yours.\n"
        "Eye color is the color of the robot's eyes, not eye lighting and not a head/neck angle. For an eye-color "
        "request, call the matching command: red vcr, blue vcb, orange vco, yellow vcy, green vcg, pink vcp, purple "
        "vcu. Never convert a color to an m0 angle. For an eye-color request, call the tool immediately without any "
        "before-call speech or progress acknowledgement. After completion, in Chinese say '已经把我的眼睛调成<颜色>啦。' "
        "Never say '眼部灯光' or '你的眼睛'.\n"
        "This tool waits for the robot completion token, except XO/Xo which return immediately. Before a waiting call "
        "returns, only say that the action is in progress. For mouse transform, call this tool and speak 'as you wish' "
        "(English) or '如你所愿' (Chinese) in the same turn without waiting; firmware sends XO/Xo after that speech. "
        "Say a waiting action is complete only when the tool returns 'completed'. If it reports failure or timeout, say that "
        "completion could not be confirmed; never claim the action completed.\n"
        "Use this tool only when the user explicitly asks for a physical robot body motion, a mouse transform, or an eye-color change. Do not "
        "call it for normal chat, self-introductions, ability descriptions, or question answering. Do not add default "
        "gestures or motions. Do not send eye expression tokens such as veH, veS, veP, veL, veT, velT, vetL, veTH, or ved; "
        "firmware applies those from speech state and reply mood. Firmware also nods (knd) or shakes the head (kwh) "
        "from yes/no meaning in the reply, after eye expressions and before other queued body motions. "
        "Do not call this tool only to nod or shake when answering yes or no, including 没错, 是的, 对, or nope. "
        "Call knd/kwh only when the user explicitly asks for that head motion. Never confuse them with gait kvtL or kvtR. Always use this "
        "tool for explicit robot body motions, mouse transform, or eye-color changes, including English "
        "requests such as sit down, stand up, go forward, turn left/right, run, crawl, nod, shake head, become a mouse, "
        "or make the eyes "
        "purple. Do not route these robot commands to smart_home. "
        "Determine reply language only from the user's latest request, never from older turns, retrieved context, "
        "examples, or tool results. If the latest request is English, every user-facing sentence before and after "
        "the tool call must be English. Head motion may use m0, knd, or kwh.\n"
        "Gait: kwkF forward, kbkF backward, ktrF run, kcrF crawl, kvtL turn left, kvtR turn right. "
        "Parameter <=200 means steps/default 3; >200 means milliseconds; turn parameter is usually degrees/default "
        "90.\n"
        "Basic: khi greet, ksit sit, kgdb good, kpu push-up, kjmp jump, d rest, kup stand, kstr stretch.\n"
        "Emotion: kang angry, khg hug, kfiv high-five, kchr cheer, knd nod (yes), kwh shake head (no), kzz sleepy.\n"
        "Skills: kbf backflip, kff frontflip, khds handstand, kmw moonwalk, krl roll, kbx boxing, kkc kick, "
        "kjumpSlide jump slide / 跳滑. Send exact uppercase letter T to call the skill that was just learned; "
        "never send veT, t, or lowercase t for this.\n"
        "Transform: I can become a mouse, meaning either the animal mouse or a computer mouse. "
        "Send exact UART code XO through this tool to transform; send exact UART code Xo (capital X, lowercase o) "
        "to return to the normal robot-dog state. These two codes are case-sensitive and different; never send xo, xO, or X0. "
        "When the user asks to become a mouse or to return to normal, call this tool with XO or Xo. The tool returns "
        "immediately. Speak 'as you wish' if the latest request is English, or '如你所愿' if it is Chinese, in that same "
        "turn. Firmware sends the UART command after that speech. Do not wait for a completion token before speaking.\n"
        "Interaction: kcmh come, khsk handshake, khu raise hand, ksnf sniff, kscrh scratch, kdg dig, kpee pee, kpd "
        "play dead.\n"
        "Head/joints: knd nod means yes; kwh shake head means no, the opposite of nod. "
        "m0 angle physically turns the neck/head (positive left, negative "
        "right, e.g. m0 45); m0 never controls color or hue. "
        "m<number> <angle>: head=0, left hand=8, right hand=9; negative is forward, positive is backward.\n"
        "Examples: sit -> ksit; run -> ktrF 3; spin/rotate/turn around -> kvtL 360; user asks to nod -> knd; "
        "user asks to shake head -> kwh; jump slide/跳滑 -> kjumpSlide; "
        "become a mouse / 变成老鼠 / 变成鼠标 -> XO; return to normal / 变回 -> Xo; "
        "replay the learned skill / 刚才学会的技能 / 调用刚才学会的技能 -> T; "
        "turn head left -> m0 45; raise left hand -> m8 -30. A spin is a turn, never forward command kwkF.",
        __robot_send_command, NULL, MCP_PROP_STR("text", "Machine command code only; no Chinese or descriptions."));
}
