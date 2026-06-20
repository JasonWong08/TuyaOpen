/**
 * @file robot_uart_voice.c
 * @brief Local ASR -> second UART command (fallback when cloud does not call MCP tools).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"

#include "robot_uart_voice.h"
#include "second_uart.h"
#include "ai_mode_free.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

typedef struct {
    const char *phrase;
    const char *cmd;
} ROBOT_VOICE_RULE_T;

/* Longer phrases first to avoid partial false matches */
static const ROBOT_VOICE_RULE_T s_voice_rules[] = {
    {"\xE5\x90\x91\xE5\x90\x8E\xE9\x80\x80", "kbk 3"},
    {"\xE5\xBE\x80\xE5\x90\x8E\xE9\x80\x80", "kbk 3"},
    {"\xE5\x90\x91\xE5\x90\x8E\xE8\xB5\xB0", "kbk 3"},
    {"\xE5\x90\x91\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"\xE5\xBE\x80\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"\xE5\x90\x91\xE5\x89\x8D\xE8\xBF\x9B", "kwkF 3"},
    {"\xE8\xB5\xB0\xE5\x87\xA0\xE6\xAD\xA5", "kwkF 3"},
    {"\xE9\x80\x80\xE5\x87\xA0\xE6\xAD\xA5", "kbk 3"},
    {"\xE5\x90\x91\xE5\x90\x8E", "kbk 3"},
    {"\xE5\xBE\x80\xE5\x89\x8D", "kwkF 3"},
    {"\xE5\x90\x8E\xE9\x80\x80", "kbk 3"},
    {"\xE5\x80\x92\xE9\x80\x80", "kbk 3"},
    {"\xE5\x89\x8D\xE8\xBF\x9B", "kwkF 3"},
    {"\xE5\xB7\xA6\xE8\xBD\xAC", "kvtL 90"},
    {"\xE5\x8F\xB3\xE8\xBD\xAC", "kvtR 90"},
    {"\xE5\x90\x91\xE5\xB7\xA6", "kvtL 90"},
    {"\xE5\x90\x91\xE5\x8F\xB3", "kvtR 90"},
    {"\xE8\xB7\x91", "ktrF 3"},
    {"\xE7\x88\xAC", "kcrF 3"},
    {"\xE5\x9D\x90\xE4\xB8\x8B", "ksit"},
    {"\xE5\x9D\x90", "ksit"},
    {"\xE7\xAB\x99\xE8\xB5\xB7\xE6\x9D\xA5", "kup"},
    {"\xE8\xB5\xB7\xE7\xAB\x8B", "kup"},
    {"\xE7\xAB\x99\xE7\xAB\x8B", "kup"},
    {"\xE4\xBC\x91\xE6\x81\xAF", "d"},
    {"\xE7\x9D\xA1\xE8\xA7\x89", "d"},
    {"\xE5\xBE\x85\xE6\x9C\xBA", "d"},
    {"\xE5\x81\x9C", "d"},
    {"\xE6\x89\x93\xE6\x8B\x9B\xE5\x91\xBC", "khi"},
    {"\xE7\x82\xB9\xE5\xA4\xB4", "knd"},
    {"\xE6\x91\x87\xE5\xA4\xB4", "kwh"},
};

static int __robot_voice_parse_steps(const char *asr)
{
    static const struct {
        const char *text;
        int value;
    } s_chinese_steps[] = {
        {"\xE5\x8D\x81", 10}, {"\xE4\xB9\x9D", 9}, {"\xE5\x85\xAB", 8}, {"\xE4\xB8\x83", 7},
        {"\xE5\x85\xAD", 6}, {"\xE4\xBA\x94", 5}, {"\xE5\x9B\x9B", 4}, {"\xE4\xB8\x89", 3},
        {"\xE4\xBA\x8C", 2}, {"\xE4\xB8\xA4", 2}, {"\xE4\xB8\x80", 1},
    };
    const char *p;
    size_t i;

    for (p = asr; p && *p; p++) {
        if (*p >= '0' && *p <= '9') {
            int steps = 0;
            while (*p >= '0' && *p <= '9') {
                steps = steps * 10 + (*p - '0');
                p++;
            }
            if (steps >= 1 && steps <= 99) {
                return steps;
            }
            break;
        }
    }

    for (i = 0; i < sizeof(s_chinese_steps) / sizeof(s_chinese_steps[0]); i++) {
        if (strstr(asr, s_chinese_steps[i].text) != NULL) {
            return s_chinese_steps[i].value;
        }
    }

    return 3;
}

static bool __robot_voice_match_walk_cmd(const char *asr, char *cmd, size_t cmd_len)
{
    static const char *s_forward_phrases[] = {
        "\xE5\x90\x91\xE5\x89\x8D\xE8\xB5\xB0", "\xE5\xBE\x80\xE5\x89\x8D\xE8\xB5\xB0",
        "\xE5\x89\x8D\xE8\xB5\xB0", "\xE5\x90\x91\xE5\x89\x8D\xE8\xBF\x9B",
        "\xE5\xBE\x80\xE5\x89\x8D", "\xE5\x89\x8D\xE8\xBF\x9B",
        "\xE8\xB5\xB0\xE5\x87\xA0\xE6\xAD\xA5",
    };
    static const char *s_backward_phrases[] = {
        "\xE5\x90\x91\xE5\x90\x8E\xE9\x80\x80", "\xE5\xBE\x80\xE5\x90\x8E\xE9\x80\x80",
        "\xE5\x90\x91\xE5\x90\x8E\xE8\xB5\xB0", "\xE9\x80\x80\xE5\x87\xA0\xE6\xAD\xA5",
        "\xE5\x90\x91\xE5\x90\x8E", "\xE5\x90\x8E\xE9\x80\x80", "\xE5\x80\x92\xE9\x80\x80",
    };
    size_t i;
    int steps;

    for (i = 0; i < sizeof(s_backward_phrases) / sizeof(s_backward_phrases[0]); i++) {
        if (strstr(asr, s_backward_phrases[i]) != NULL) {
            steps = __robot_voice_parse_steps(asr);
            snprintf(cmd, cmd_len, "kbk %d", steps);
            return true;
        }
    }

    for (i = 0; i < sizeof(s_forward_phrases) / sizeof(s_forward_phrases[0]); i++) {
        if (strstr(asr, s_forward_phrases[i]) != NULL) {
            steps = __robot_voice_parse_steps(asr);
            snprintf(cmd, cmd_len, "kwkF %d", steps);
            return true;
        }
    }

    if (strstr(asr, "\xE8\x8A\x82\xE5\xA5\x8F") != NULL &&
        (strstr(asr, "\xE6\xAD\xA5") != NULL || strstr(asr, "\xE9\x83\xA8") != NULL)) {
        steps = __robot_voice_parse_steps(asr);
        snprintf(cmd, cmd_len, "kwkF %d", steps);
        return true;
    }

    return false;
}

static bool __robot_voice_match_cmd(const char *asr, char *cmd, size_t cmd_len)
{
    size_t i;

    if (!asr || asr[0] == '\0' || !cmd || cmd_len == 0) {
        return false;
    }

    if (__robot_voice_match_walk_cmd(asr, cmd, cmd_len)) {
        return true;
    }

    for (i = 0; i < sizeof(s_voice_rules) / sizeof(s_voice_rules[0]); i++) {
        if (strstr(asr, s_voice_rules[i].phrase) != NULL) {
            snprintf(cmd, cmd_len, "%s", s_voice_rules[i].cmd);
            return true;
        }
    }
    return false;
}

static bool __robot_voice_is_exit_phrase(const char *asr)
{
    static const char *s_exit_phrases[] = {
        "\xE7\xBB\x93\xE6\x9D\x9F\xE5\xAF\xB9\xE8\xAF\x9D",
        "\xE9\x80\x80\xE5\x87\xBA\xE5\xAF\xB9\xE8\xAF\x9D",
        "\xE5\x81\x9C\xE6\xAD\xA2\xE5\xAF\xB9\xE8\xAF\x9D",
        "\xE7\xBB\x93\xE6\x9D\x9F\xE8\x81\x8A\xE5\xA4\xA9",
        "\xE9\x80\x80\xE5\x87\xBA\xE8\x81\x8A\xE5\xA4\xA9",
        "\xE7\xBB\x93\xE6\x9D\x9F",
        "\xE5\x86\x8D\xE8\xA7\x81",
    };
    size_t i;

    if (!asr || asr[0] == '\0') {
        return false;
    }

    for (i = 0; i < sizeof(s_exit_phrases) / sizeof(s_exit_phrases[0]); i++) {
        if (strstr(asr, s_exit_phrases[i]) != NULL) {
            return true;
        }
    }

    if (strlen(asr) <= 9 && strstr(asr, "\xE5\xAF\xB9\xE8\xAF\x9D") != NULL) {
        return true;
    }

    return false;
}

void robot_uart_voice_on_asr(const char *asr_text)
{
    char cmd[16] = {0};
    OPERATE_RET rt;

    if (!__robot_voice_match_cmd(asr_text, cmd, sizeof(cmd))) {
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
    if (__robot_voice_is_exit_phrase(text)) {
        ai_mode_free_request_exit();
    }

    robot_uart_voice_on_asr(text);
}

static void __cli_robot_uart_cmd(int argc, char *argv[])
{
    const char *cmd = (argc >= 2) ? argv[1] : "kbk 3";
    OPERATE_RET rt  = second_uart_send_test(cmd);

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
