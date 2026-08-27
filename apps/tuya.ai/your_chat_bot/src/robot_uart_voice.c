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
#include "quaddle_robot_bridge.h"
#include "second_uart.h"
#include "ai_mode_free.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#define ROBOT_UART_CMD_LISTEN     "vetL"
#define ROBOT_UART_CMD_TALK       "velT"
#define ROBOT_UART_CMD_TALK_END   "vet"
#define ROBOT_UART_CMD_EFFECT_END "ved"

static bool s_expect_vet = false;
static bool s_eye_color_reply_pending = false;
static bool s_eye_color_mcp_completed = false;
static bool s_eye_color_tts_suppressed = false;
static char s_eye_color_pending_cmd[16] = {0};

static void __robot_uart_voice_send_now(const char *cmd)
{
    OPERATE_RET rt;

    rt = second_uart_send_string_force(cmd);
    if (rt != OPRT_OK) {
        PR_ERR("robot voice: send \"%s\" failed %d", cmd, rt);
        return;
    }

    PR_NOTICE("robot voice: sent UART \"%s\"", cmd);
}

void ai_app_on_record_start(void)
{
    /* vetL already ends talk on the MCU. Ignore a later PLAY_END so it
     * does not send a redundant vet after listen animation has started. */
    s_expect_vet = false;
    __robot_uart_voice_send_now(ROBOT_UART_CMD_LISTEN);
}

void ai_app_on_free_mode_exit(void)
{
    /* The free-mode state handler runs before the app event callback. Clear
     * this flag so the same PLAY_END does not overwrite ved with vet. */
    s_expect_vet = false;
    __robot_uart_voice_send_now(ROBOT_UART_CMD_EFFECT_END);
}

void robot_uart_voice_on_event(const AI_NOTIFY_EVENT_T *event)
{
    if (!event) {
        return;
    }

    switch (event->type) {
    case AI_USER_EVT_TTS_PRE:
        s_expect_vet = true;
        __robot_uart_voice_send_now(ROBOT_UART_CMD_TALK);
        break;
    case AI_USER_EVT_TTS_ABORT:
    case AI_USER_EVT_PLAY_END:
        if (!s_expect_vet) {
            break;
        }
        s_expect_vet = false;
        __robot_uart_voice_send_now(ROBOT_UART_CMD_TALK_END);
        break;
    default:
        break;
    }
}

typedef struct {
    const char *phrase;
    const char *cmd;
} ROBOT_VOICE_RULE_T;

static char __robot_voice_ascii_tolower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }

    return ch;
}

static bool __robot_voice_contains(const char *text, const char *phrase)
{
    size_t text_len;
    size_t phrase_len;
    size_t i;
    size_t j;

    if (!text || !phrase || phrase[0] == '\0') {
        return false;
    }

    text_len = strlen(text);
    phrase_len = strlen(phrase);
    if (phrase_len > text_len) {
        return false;
    }

    for (i = 0; i <= text_len - phrase_len; i++) {
        for (j = 0; j < phrase_len; j++) {
            if (__robot_voice_ascii_tolower(text[i + j]) != __robot_voice_ascii_tolower(phrase[j])) {
                break;
            }
        }
        if (j == phrase_len) {
            return true;
        }
    }

    return false;
}

/* Longer phrases first to avoid partial false matches */
static const ROBOT_VOICE_RULE_T s_voice_rules[] = {
    {"\xE5\x90\x91\xE5\x90\x8E\xE9\x80\x80", "kbkF 3"},
    {"move backward", "kbkF 3"},
    {"walk backward", "kbkF 3"},
    {"go backward", "kbkF 3"},
    {"\xE5\xBE\x80\xE5\x90\x8E\xE9\x80\x80", "kbkF 3"},
    {"move back", "kbkF 3"},
    {"walk back", "kbkF 3"},
    {"go back", "kbkF 3"},
    {"\xE5\x90\x91\xE5\x90\x8E\xE8\xB5\xB0", "kbkF 3"},
    {"step back", "kbkF 3"},
    {"\xE5\x90\x91\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"move forward", "kwkF 3"},
    {"walk forward", "kwkF 3"},
    {"go forward", "kwkF 3"},
    {"go forword", "kwkF 3"},
    {"\xE5\xBE\x80\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"move ahead", "kwkF 3"},
    {"walk ahead", "kwkF 3"},
    {"go ahead", "kwkF 3"},
    {"\xE5\x89\x8D\xE8\xB5\xB0", "kwkF 3"},
    {"step forward", "kwkF 3"},
    {"\xE5\x90\x91\xE5\x89\x8D\xE8\xBF\x9B", "kwkF 3"},
    {"forward", "kwkF 3"},
    {"\xE8\xB5\xB0\xE5\x87\xA0\xE6\xAD\xA5", "kwkF 3"},
    {"walk a few steps", "kwkF 3"},
    {"\xE9\x80\x80\xE5\x87\xA0\xE6\xAD\xA5", "kbkF 3"},
    {"back a few steps", "kbkF 3"},
    {"\xE5\x90\x91\xE5\x90\x8E", "kbkF 3"},
    {"backward", "kbkF 3"},
    {"\xE5\xBE\x80\xE5\x89\x8D", "kwkF 3"},
    {"ahead", "kwkF 3"},
    {"\xE5\x90\x8E\xE9\x80\x80", "kbkF 3"},
    {"\xE5\x80\x92\xE9\x80\x80", "kbkF 3"},
    {"\xE5\x89\x8D\xE8\xBF\x9B", "kwkF 3"},
    {"\xE5\xB7\xA6\xE8\xBD\xAC", "kvtL 90"},
    {"turn left", "kvtL 90"},
    {"left turn", "kvtL 90"},
    {"\xE5\x8F\xB3\xE8\xBD\xAC", "kvtR 90"},
    {"turn right", "kvtR 90"},
    {"right turn", "kvtR 90"},
    {"\xE5\x90\x91\xE5\xB7\xA6", "kvtL 90"},
    {"to the left", "kvtL 90"},
    {"\xE5\x90\x91\xE5\x8F\xB3", "kvtR 90"},
    {"to the right", "kvtR 90"},
    {"\xE8\xB7\x91", "ktrF 3"},
    {"run", "ktrF 3"},
    {"\xE7\x88\xAC", "kcrF 3"},
    {"crawl", "kcrF 3"},
    {"\xE5\x9D\x90\xE4\xB8\x8B", "ksit"},
    {"sit down", "ksit"},
    {"\xE5\x9D\x90", "ksit"},
    {"sit", "ksit"},
    {"\xE4\xBC\x91\xE6\x81\xAF", "d"},
    {"take a rest", "d"},
    {"rest", "d"},
    {"\xE7\x9D\xA1\xE8\xA7\x89", "d"},
    {"go to sleep", "d"},
    {"sleep", "d"},
    {"\xE5\xBE\x85\xE6\x9C\xBA", "d"},
    {"standby", "d"},
    {"stand by", "d"},
    {"\xE5\x81\x9C", "d"},
    {"stop", "d"},
    {"\xE7\xAB\x99\xE8\xB5\xB7\xE6\x9D\xA5", "kup"},
    {"stand up", "kup"},
    {"get up", "kup"},
    {"\xE8\xB5\xB7\xE7\xAB\x8B", "kup"},
    {"rise", "kup"},
    {"\xE7\xAB\x99\xE7\xAB\x8B", "kup"},
    {"stand", "kup"},
    {"\xE6\x89\x93\xE6\x8B\x9B\xE5\x91\xBC", "khi"},
    {"say hello", "khi"},
    {"greet", "khi"},
    {"hello", "khi"},
    {"\xE7\x82\xB9\xE5\xA4\xB4", "knd"},
    {"nod", "knd"},
    {"\xE6\x91\x87\xE5\xA4\xB4", "kwh"},
    {"shake head", "kwh"},
    {"shake your head", "kwh"},
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
    static const struct {
        const char *text;
        int value;
    } s_english_steps[] = {
        {"ten", 10},   {"nine", 9}, {"eight", 8}, {"seven", 7}, {"six", 6},
        {"five", 5},  {"four", 4}, {"three", 3}, {"two", 2},   {"one", 1},
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

    for (i = 0; i < sizeof(s_english_steps) / sizeof(s_english_steps[0]); i++) {
        if (__robot_voice_contains(asr, s_english_steps[i].text)) {
            return s_english_steps[i].value;
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
        "move forward", "walk forward", "go forward", "go forword", "move ahead", "walk ahead", "go ahead",
        "step forward", "forward",
    };
    static const char *s_backward_phrases[] = {
        "\xE5\x90\x91\xE5\x90\x8E\xE9\x80\x80", "\xE5\xBE\x80\xE5\x90\x8E\xE9\x80\x80",
        "\xE5\x90\x91\xE5\x90\x8E\xE8\xB5\xB0", "\xE9\x80\x80\xE5\x87\xA0\xE6\xAD\xA5",
        "\xE5\x90\x91\xE5\x90\x8E", "\xE5\x90\x8E\xE9\x80\x80", "\xE5\x80\x92\xE9\x80\x80",
        "move backward", "walk backward", "go backward", "move back", "walk back", "go back", "step back",
        "backward",
    };
    size_t i;
    int steps;

    for (i = 0; i < sizeof(s_backward_phrases) / sizeof(s_backward_phrases[0]); i++) {
        if (__robot_voice_contains(asr, s_backward_phrases[i])) {
            steps = __robot_voice_parse_steps(asr);
            snprintf(cmd, cmd_len, "kbkF %d", steps);
            return true;
        }
    }

    for (i = 0; i < sizeof(s_forward_phrases) / sizeof(s_forward_phrases[0]); i++) {
        if (__robot_voice_contains(asr, s_forward_phrases[i])) {
            steps = __robot_voice_parse_steps(asr);
            snprintf(cmd, cmd_len, "kwkF %d", steps);
            return true;
        }
    }

    if ((__robot_voice_contains(asr, "\xE8\x8A\x82\xE5\xA5\x8F") || __robot_voice_contains(asr, "rhythm") ||
         __robot_voice_contains(asr, "beat")) &&
        (__robot_voice_contains(asr, "\xE6\xAD\xA5") || __robot_voice_contains(asr, "\xE9\x83\xA8") ||
         __robot_voice_contains(asr, "step"))) {
        steps = __robot_voice_parse_steps(asr);
        snprintf(cmd, cmd_len, "kwkF %d", steps);
        return true;
    }

    return false;
}

static bool __robot_voice_match_eye_color_cmd(const char *asr, char *cmd, size_t cmd_len)
{
    static const ROBOT_VOICE_RULE_T s_eye_color_rules[] = {
        {"\xE7\xB2\x89\xE7\xBA\xA2\xE8\x89\xB2", "vcp"},
        {"\xE7\xB2\x89\xE7\xBA\xA2", "vcp"},
        {"\xE7\xBA\xA2\xE8\x89\xB2", "vcr"},
        {"\xE8\x93\x9D\xE8\x89\xB2", "vcb"},
        {"\xE6\xA9\x99\xE8\x89\xB2", "vco"},
        {"\xE6\xA9\x98\xE8\x89\xB2", "vco"},
        {"\xE9\xBB\x84\xE8\x89\xB2", "vcy"},
        {"\xE7\xBB\xBF\xE8\x89\xB2", "vcg"},
        {"\xE7\xB2\x89\xE8\x89\xB2", "vcp"},
        {"\xE7\xB4\xAB\xE8\x89\xB2", "vcu"},
        {"orange", "vco"},
        {"yellow", "vcy"},
        {"purple", "vcu"},
        {"green", "vcg"},
        {"blue", "vcb"},
        {"pink", "vcp"},
        {"red", "vcr"},
        {"\xE7\xBA\xA2", "vcr"},
        {"\xE8\x93\x9D", "vcb"},
        {"\xE6\xA9\x99", "vco"},
        {"\xE6\xA9\x98", "vco"},
        {"\xE9\xBB\x84", "vcy"},
        {"\xE7\xBB\xBF", "vcg"},
        {"\xE7\xB2\x89", "vcp"},
        {"\xE7\xB4\xAB", "vcu"},
    };
    static const char *s_eye_targets[] = {
        "\xE7\x9C\xBC\xE7\x9D\x9B",
        "\xE7\x9C\xBC\xE8\x89\xB2",
        "eye color",
        "eyes",
        "eye",
    };
    static const char *s_change_words[] = {
        "\xE8\xAE\xBE\xE7\xBD\xAE",
        "\xE6\x94\xB9",
        "\xE8\xB0\x83",
        "\xE6\x8D\xA2",
        "\xE5\x8F\x98",
        "change",
        "set",
        "turn",
        "make",
        "switch",
        "adjust",
        "modify",
        "color",
        "colour",
    };
    size_t i;
    bool has_eye_target = false;
    bool has_change_word = false;

    for (i = 0; i < sizeof(s_eye_targets) / sizeof(s_eye_targets[0]); i++) {
        if (__robot_voice_contains(asr, s_eye_targets[i])) {
            has_eye_target = true;
            break;
        }
    }
    for (i = 0; i < sizeof(s_change_words) / sizeof(s_change_words[0]); i++) {
        if (__robot_voice_contains(asr, s_change_words[i])) {
            has_change_word = true;
            break;
        }
    }
    if (!has_eye_target || !has_change_word) {
        return false;
    }

    for (i = 0; i < sizeof(s_eye_color_rules) / sizeof(s_eye_color_rules[0]); i++) {
        if (__robot_voice_contains(asr, s_eye_color_rules[i].phrase)) {
            snprintf(cmd, cmd_len, "%s", s_eye_color_rules[i].cmd);
            return true;
        }
    }

    return false;
}

static bool __robot_voice_match_cmd(const char *asr, char *cmd, size_t cmd_len)
{
    size_t i;

    if (!asr || asr[0] == '\0' || !cmd || cmd_len == 0) {
        return false;
    }

    if (__robot_voice_match_eye_color_cmd(asr, cmd, cmd_len)) {
        return true;
    }

    if (__robot_voice_match_walk_cmd(asr, cmd, cmd_len)) {
        return true;
    }

    for (i = 0; i < sizeof(s_voice_rules) / sizeof(s_voice_rules[0]); i++) {
        if (__robot_voice_contains(asr, s_voice_rules[i].phrase)) {
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
        "\xE6\x8B\x9C\xE6\x8B\x9C",
        "end conversation",
        "exit conversation",
        "stop conversation",
        "end chat",
        "exit chat",
        "goodbye",
        "bye bye",
        "bye",
        "see you",
        "talk to you later",
        "that's all",
        "that is all",
    };
    size_t i;

    if (!asr || asr[0] == '\0') {
        return false;
    }

    for (i = 0; i < sizeof(s_exit_phrases) / sizeof(s_exit_phrases[0]); i++) {
        if (__robot_voice_contains(asr, s_exit_phrases[i])) {
            return true;
        }
    }

    if (strlen(asr) <= 9 && __robot_voice_contains(asr, "\xE5\xAF\xB9\xE8\xAF\x9D")) {
        return true;
    }

    return false;
}

static bool __robot_voice_is_compound_command(const char *asr)
{
    if (!asr || asr[0] == '\0') {
        return false;
    }

    if (__robot_voice_contains(asr, "\xE7\x84\xB6\xE5\x90\x8E") ||
        __robot_voice_contains(asr, "\xE6\x8E\xA5\xE7\x9D\x80") ||
        __robot_voice_contains(asr, "and then") ||
        __robot_voice_contains(asr, "then")) {
        return true;
    }

    return __robot_voice_contains(asr, "\xE5\x85\x88") && __robot_voice_contains(asr, "\xE5\x86\x8D");
}

void robot_uart_voice_on_asr(const char *asr_text)
{
    char cmd[16] = {0};
    OPERATE_RET rt;

    if (__robot_voice_is_compound_command(asr_text)) {
        PR_NOTICE("robot voice: compound ASR skipped for MCP planning \"%s\"", asr_text);
        return;
    }

    if (!__robot_voice_match_cmd(asr_text, cmd, sizeof(cmd))) {
        return;
    }

    rt = quaddle_robot_bridge_queue_ai_command(cmd, "ASR");
    if (rt != OPRT_OK) {
        PR_ERR("robot voice: queue \"%s\" failed %d", cmd, rt);
        return;
    }

    PR_NOTICE("robot voice: ASR \"%s\" -> queued UART \"%s\"", asr_text, cmd);
}

void ai_app_on_asr_result(const char *text)
{
    char eye_color_cmd[16] = {0};

    s_eye_color_reply_pending = __robot_voice_match_eye_color_cmd(text, eye_color_cmd, sizeof(eye_color_cmd));
    s_eye_color_mcp_completed = false;
    s_eye_color_tts_suppressed =
        s_eye_color_reply_pending && (__robot_voice_contains(text, "\xE7\x9C\xBC\xE7\x9D\x9B") ||
                                      __robot_voice_contains(text, "\xE7\x9C\xBC\xE8\x89\xB2"));
    if (s_eye_color_reply_pending) {
        snprintf(s_eye_color_pending_cmd, sizeof(s_eye_color_pending_cmd), "%s", eye_color_cmd);
    } else {
        s_eye_color_pending_cmd[0] = '\0';
    }

    if (__robot_voice_is_exit_phrase(text)) {
        ai_mode_free_request_exit();
    }

    robot_uart_voice_on_asr(text);
}

bool robot_uart_voice_get_pending_eye_color_command(char *cmd, size_t cmd_len)
{
    if (!s_eye_color_reply_pending || s_eye_color_pending_cmd[0] == '\0' || !cmd || cmd_len == 0) {
        return false;
    }

    snprintf(cmd, cmd_len, "%s", s_eye_color_pending_cmd);
    return true;
}

void robot_uart_voice_mark_eye_color_mcp_completed(void)
{
    if (s_eye_color_reply_pending) {
        s_eye_color_mcp_completed = true;
    }
}

bool ai_app_should_suppress_tts_audio(void)
{
    return s_eye_color_tts_suppressed;
}

void ai_app_filter_nlg_text(char *text, bool eof)
{
    static const char second_person[] = "\xE4\xBD\xA0\xE7\x9A\x84";
    static const char first_person[]  = "\xE6\x88\x91\xE7\x9A\x84";
    char             *match;

    if (s_eye_color_reply_pending && text) {
        if (strstr(text, "\xE7\x9C\xBC\xE9\x83\xA8\xE7\x81\xAF\xE5\x85\x89") != NULL) {
            text[0] = '\0';
            PR_NOTICE("robot voice: removed incorrect eye-lighting NLG text");
        }
        while ((match = strstr(text, second_person)) != NULL) {
            memcpy(match, first_person, sizeof(first_person) - 1);
            text = match + sizeof(first_person) - 1;
            PR_NOTICE("robot voice: corrected eye-color reply to first person");
        }
        if ((s_eye_color_mcp_completed && text[0] != '\0') ||
            strstr(text, "\xE5\xB7\xB2\xE7\xBB\x8F") != NULL) {
            s_eye_color_tts_suppressed = false;
            PR_NOTICE("robot voice: allow completed eye-color TTS reply");
        }
    }

    if (eof) {
        s_eye_color_reply_pending = false;
        s_eye_color_mcp_completed = false;
        s_eye_color_tts_suppressed = false;
        s_eye_color_pending_cmd[0] = '\0';
    }
}

static void __cli_robot_uart_cmd(int argc, char *argv[])
{
    const char *cmd = (argc >= 2) ? argv[1] : "kbkF 3";
    OPERATE_RET rt  = second_uart_send_test(cmd);

    if (rt == OPRT_OK) {
        tal_cli_echo("robot_uart: sent OK\r\n");
    } else {
        tal_cli_echo("robot_uart: send failed\r\n");
    }
}

static const cli_cmd_t s_robot_uart_cli[] = {
    {"robot_uart", "robot_uart [cmd]  e.g. robot_uart kbkF 3  (UART1 TX17->dog RX)", __cli_robot_uart_cmd},
};

OPERATE_RET robot_uart_voice_init(void)
{
    OPERATE_RET rt;

    rt = second_uart_init();
    if (rt != OPRT_OK) {
        PR_WARN("robot voice: early uart init %d (retry on first command)", rt);
    }

    tal_cli_cmd_register(s_robot_uart_cli, 1);

    PR_NOTICE("robot voice: ASR->UART1 enabled; record/talk cues vetL, velT, vet; CLI: robot_uart [cmd]");
    return OPRT_OK;
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
