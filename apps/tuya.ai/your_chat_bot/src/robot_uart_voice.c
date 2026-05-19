/**
 * @file robot_uart_voice.c
 * @brief Local ASR -> second UART command (fallback when cloud does not call MCP tools).
 */

#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"

#include "robot_uart_voice.h"
#include "second_uart.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

typedef struct {
    const char *phrase;
    const char *cmd;
} ROBOT_VOICE_RULE_T;

/* Longer phrases first to avoid partial false matches */
static const ROBOT_VOICE_RULE_T s_voice_rules[] = {
    {"向后退", "kbk 3"},
    {"往后退", "kbk 3"},
    {"向后走", "kbk 3"},
    {"向前走", "kwkF 3"},
    {"往前走", "kwkF 3"},
    {"前走", "kwkF 3"},
    {"向前进", "kwkF 3"},
    {"走几步", "kwkF 3"},
    {"退几步", "kbk 3"},
    {"向后", "kbk 3"},
    {"往前", "kwkF 3"},
    {"后退", "kbk 3"},
    {"倒退", "kbk 3"},
    {"前进", "kwkF 3"},
    {"左转", "kvtL 90"},
    {"右转", "kvtR 90"},
    {"向左", "kvtL 90"},
    {"向右", "kvtR 90"},
    {"跑", "ktrF 3"},
    {"爬", "kcrF 3"},
    {"坐下", "ksit"},
    {"坐", "ksit"},
    {"站起来", "kup"},
    {"起立", "kup"},
    {"站立", "kup"},
    {"休息", "d"},
    {"睡觉", "d"},
    {"待机", "d"},
    {"停", "d"},
    {"打招呼", "khi"},
    {"点头", "knd"},
    {"摇头", "kwh"},
};

static const char *__robot_voice_match_cmd(const char *asr)
{
    size_t i;

    if (!asr || asr[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < sizeof(s_voice_rules) / sizeof(s_voice_rules[0]); i++) {
        if (strstr(asr, s_voice_rules[i].phrase) != NULL) {
            return s_voice_rules[i].cmd;
        }
    }
    return NULL;
}

void robot_uart_voice_on_asr(const char *asr_text)
{
    const char *cmd;
    OPERATE_RET rt;

    cmd = __robot_voice_match_cmd(asr_text);
    if (!cmd) {
        return;
    }

    rt = second_uart_init();
    if (rt != OPRT_OK) {
        PR_ERR("robot voice: uart init failed %d", rt);
        return;
    }

    rt = second_uart_send_string(cmd);
    if (rt != OPRT_OK) {
        PR_ERR("robot voice: send \"%s\" failed %d", cmd, rt);
        return;
    }

    PR_NOTICE("robot voice: ASR \"%s\" -> UART \"%s\"", asr_text, cmd);
}

void ai_app_on_asr_result(const char *text)
{
    robot_uart_voice_on_asr(text);
}

static void __cli_robot_uart_cmd(int argc, char *argv[])
{
    const char *cmd = (argc >= 2) ? argv[1] : "kbk 3";
    OPERATE_RET   rt  = second_uart_send_test(cmd);

    if (rt == OPRT_OK) {
        tal_cli_echo("robot_uart: sent OK\r\n");
    } else {
        tal_cli_echo("robot_uart: send failed\r\n");
    }
}

static const cli_cmd_t s_robot_uart_cli[] = {
    {"robot_uart", "robot_uart [cmd]  e.g. robot_uart kbk 3  (UART1 TX17->dog RX)", __cli_robot_uart_cmd},
};

OPERATE_RET robot_uart_voice_init(void)
{
    OPERATE_RET rt;

    rt = second_uart_init();
    if (rt != OPRT_OK) {
        PR_WARN("robot voice: early uart init %d (retry on first command)", rt);
    }

    tal_cli_cmd_register(s_robot_uart_cli, 1);

    PR_NOTICE("robot voice: ASR->UART1 enabled; CLI: robot_uart [cmd]");
    return OPRT_OK;
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
