/**
 * @file ai_mcp_robot_tools.c
 * @brief your_chat_bot-only MCP tool self.robot.send_command (second UART).
 *
 * Compiled only when CONFIG_ENABLE_CHAT_BOT_ROBOT_SECOND_UART=y (see CMakeLists.txt).
 */

#include <string.h>

#include "tal_api.h"

#include "ai_mcp_server.h"

#if !defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) || !ENABLE_CHAT_BOT_ROBOT_SECOND_UART
#error "ai_mcp_robot_tools.c must be built with ENABLE_CHAT_BOT_ROBOT_SECOND_UART=1"
#endif

#include "second_uart.h"

static OPERATE_RET __robot_send_command(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val,
                                        void *user_data)
{
    const char *text       = NULL;
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

    OPERATE_RET uart_rt = second_uart_init();
    if (uart_rt != OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "uart init failed");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : uart_rt;
    }

    uart_rt = second_uart_send_string(text);
    if (uart_rt != OPRT_OK) {
        mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "uart send failed");
        return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : uart_rt;
    }

    PR_NOTICE("MCP robot send_command \"%s\"", text);

    mcp_str_rt = ai_mcp_return_value_set_str(ret_val, "command sent successfully");
    return (mcp_str_rt != OPRT_OK) ? mcp_str_rt : OPRT_OK;
}

/**
 * @brief Strong symbol: overrides weak default in ai_mcp_tools.c (only linked into your_chat_bot).
 */
OPERATE_RET ai_mcp_app_tools_register(void)
{
    return AI_MCP_TOOL_ADD(
        "self.robot.send_command",
        "机器狗物理动作指令(非屏幕表情)。参数仅含代码，勿含中文。\n"
        "【铁律】机器狗身体动作必调此工具！头部转动可用 m0、kwh（勿用 LCD 表情工具）。\n"
        "【步态】kwkF进 kbk退 ktrF跑 kcrF爬 kvtL左转 kvtR右转 "
        "(参≤200为步数或默认3，>200为毫秒；转向参常为度数默认90)\n"
        "【基本】khi招呼 ksit坐 kgdb乖 kpu俯卧撑 kjmp跳 d休 kup起 kstr伸\n"
        "【情感】kang怒 khg抱 kfiv击掌 kchr庆 knd点头 kwh摇头 kzz困\n"
        "【技能】kbf后翻 kff前翻 khds倒立 kmw太空步 krl翻滚 kbx拳击 kkc踢\n"
        "【互动】kcmh来 khsk握手 khu举手 ksnf嗅 kscrh抓 kdg挖 kpee尿 kpd装死\n"
        "【头部】kwh摇头/歪头 knd点头 m0转头(正=左负=右，如 m0 45 头左转)\n"
        "【关节】m<号> <角> 头0左手8右手9 负=前正=后\n"
        "【范例】坐下→ksit 跑→ktrF 3 歪头→kwh 头左转→m0 45 举左手→m8 -30",
        __robot_send_command,
        NULL,
        MCP_PROP_STR("text", "Machine command code only; no Chinese or descriptions."));
}
