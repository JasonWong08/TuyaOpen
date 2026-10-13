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
#define ROBOT_UART_CMD_TALK_HAPPY "veTH"
#define ROBOT_UART_CMD_TALK_SAD   "veTS"
#define ROBOT_UART_CMD_TALK_SQUINT "veTP"
#define ROBOT_UART_CMD_MOOD_HAPPY "veH"
#define ROBOT_UART_CMD_MOOD_SAD   "veS"
#define ROBOT_UART_CMD_MOOD_SQUINT "veP"
#define ROBOT_UART_NLG_MOOD_MAX   256

typedef enum {
    ROBOT_EYE_MOOD_NONE = 0,
    ROBOT_EYE_MOOD_HAPPY,
    ROBOT_EYE_MOOD_SAD,
    ROBOT_EYE_MOOD_SQUINT,
} ROBOT_EYE_MOOD_E;

typedef enum {
    ROBOT_REPLY_LANGUAGE_UNKNOWN = 0,
    ROBOT_REPLY_LANGUAGE_ENGLISH,
    ROBOT_REPLY_LANGUAGE_CHINESE,
} ROBOT_REPLY_LANGUAGE_E;

typedef enum {
    ROBOT_REPLY_STANCE_NONE = 0,
    ROBOT_REPLY_STANCE_YES,
    ROBOT_REPLY_STANCE_NO,
} ROBOT_REPLY_STANCE_E;

static bool s_expect_vet = false;
static bool s_talking = false;
static ROBOT_EYE_MOOD_E s_reply_mood = ROBOT_EYE_MOOD_NONE;
static ROBOT_EYE_MOOD_E s_applied_mood = ROBOT_EYE_MOOD_NONE;
static ROBOT_REPLY_STANCE_E s_reply_stance = ROBOT_REPLY_STANCE_NONE;
static bool s_stance_sent = false;
static bool s_explicit_head_gesture = false;
static bool s_listen_pending = false;
static char   s_nlg_mood_buf[ROBOT_UART_NLG_MOOD_MAX];
static size_t s_nlg_mood_len = 0;
static bool s_eye_color_reply_pending = false;
static bool s_eye_color_mcp_completed = false;
static bool s_eye_color_tts_suppressed = false;
static char s_eye_color_pending_cmd[16] = {0};
static ROBOT_REPLY_LANGUAGE_E s_expected_reply_language = ROBOT_REPLY_LANGUAGE_UNKNOWN;

static void __robot_uart_voice_reset_reply_mood(void);
static void __robot_uart_voice_on_text_stream(const AI_NOTIFY_TEXT_T *text, bool is_start);
static void __robot_uart_voice_on_emotion(const AI_NOTIFY_EMO_T *emo);
static void __robot_uart_voice_send_talk_with_mood(void);
static void __robot_uart_voice_send_mood_only(ROBOT_EYE_MOOD_E mood);
static void __robot_uart_voice_clear_applied_mood(void);
static void __robot_uart_voice_queue_reply_stance(void);
static ROBOT_REPLY_STANCE_E __robot_uart_stance_from_text(const char *text);

static void __robot_voice_count_language_signals(const char *text, size_t *ascii_letters, size_t *cjk_chars)
{
    const uint8_t *p = (const uint8_t *)text;

    *ascii_letters = 0;
    *cjk_chars = 0;
    if (!text) {
        return;
    }

    while (*p != '\0') {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            (*ascii_letters)++;
            p++;
            continue;
        }

        /* Chinese characters U+3400..U+9FFF encoded as three-byte UTF-8. */
        if (p[1] != '\0' && p[2] != '\0' && *p >= 0xE3 && *p <= 0xE9 &&
            (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t codepoint = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                                 (uint32_t)(p[2] & 0x3F);
            if (codepoint >= 0x3400 && codepoint <= 0x9FFF) {
                (*cjk_chars)++;
            }
            p += 3;
            continue;
        }

        p++;
    }
}

static ROBOT_REPLY_LANGUAGE_E __robot_voice_detect_language(const char *text)
{
    size_t ascii_letters;
    size_t cjk_chars;

    __robot_voice_count_language_signals(text, &ascii_letters, &cjk_chars);
    if (cjk_chars > 0 && cjk_chars >= ascii_letters) {
        return ROBOT_REPLY_LANGUAGE_CHINESE;
    }
    if (ascii_letters > 0) {
        return ROBOT_REPLY_LANGUAGE_ENGLISH;
    }
    return ROBOT_REPLY_LANGUAGE_UNKNOWN;
}

static bool __robot_voice_nlg_language_mismatch(const char *text)
{
    size_t ascii_letters;
    size_t cjk_chars;

    if (s_expected_reply_language == ROBOT_REPLY_LANGUAGE_UNKNOWN || !text || text[0] == '\0') {
        return false;
    }

    __robot_voice_count_language_signals(text, &ascii_letters, &cjk_chars);

    /* Require a clear dominance so quoted words and machine command codes do not cause false positives. */
    if (s_expected_reply_language == ROBOT_REPLY_LANGUAGE_ENGLISH) {
        return cjk_chars >= 2 && cjk_chars > ascii_letters * 2;
    }
    return ascii_letters >= 4 && ascii_letters > cjk_chars * 2;
}

static void __robot_uart_voice_send_now(const char *cmd)
{
    OPERATE_RET rt;

    if (quaddle_robot_bridge_mouse_exclusive()) {
        PR_NOTICE("robot voice: hold \"%s\" until mouse transform finishes", cmd);
        if (cmd && strcmp(cmd, ROBOT_UART_CMD_LISTEN) == 0) {
            s_listen_pending = true;
        }
        return;
    }

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
    s_reply_stance = ROBOT_REPLY_STANCE_NONE;
    s_stance_sent = false;
    s_explicit_head_gesture = false;
    quaddle_robot_bridge_hold_ai_body_commands();
    __robot_uart_voice_clear_applied_mood();
    __robot_uart_voice_send_now(ROBOT_UART_CMD_LISTEN);
}

void ai_app_on_free_mode_exit(void)
{
    /* The free-mode state handler runs before the app event callback. Clear
     * this flag so the same PLAY_END does not overwrite ved with vet. */
    s_expect_vet = false;
    s_talking = false;
    s_applied_mood = ROBOT_EYE_MOOD_NONE;
    s_stance_sent = false;
    __robot_uart_voice_reset_reply_mood();
    quaddle_robot_bridge_release_ai_body_hold();
    __robot_uart_voice_send_now(ROBOT_UART_CMD_EFFECT_END);
}

void robot_uart_voice_on_event(const AI_NOTIFY_EVENT_T *event)
{
    if (!event) {
        return;
    }

    switch (event->type) {
    case AI_USER_EVT_TEXT_STREAM_START:
        __robot_uart_voice_on_text_stream((const AI_NOTIFY_TEXT_T *)event->data, true);
        break;
    case AI_USER_EVT_TEXT_STREAM_DATA:
        __robot_uart_voice_on_text_stream((const AI_NOTIFY_TEXT_T *)event->data, false);
        break;
    case AI_USER_EVT_LLM_EMOTION:
    case AI_USER_EVT_EMOTION:
        __robot_uart_voice_on_emotion((const AI_NOTIFY_EMO_T *)event->data);
        break;
    case AI_USER_EVT_CHAT_BREAK:
        s_talking = false;
        s_stance_sent = false;
        __robot_uart_voice_reset_reply_mood();
        __robot_uart_voice_clear_applied_mood();
        quaddle_robot_bridge_release_ai_body_hold();
        break;
    case AI_USER_EVT_TTS_PRE:
        s_expect_vet = true;
        s_talking = true;
        __robot_uart_voice_send_talk_with_mood();
        __robot_uart_voice_queue_reply_stance();
        quaddle_robot_bridge_release_ai_body_hold();
        break;
    case AI_USER_EVT_TTS_ABORT:
    case AI_USER_EVT_PLAY_END:
        s_talking = false;
        if (s_expect_vet) {
            s_expect_vet = false;
            if (!quaddle_robot_bridge_has_deferred_mouse_transform()) {
                __robot_uart_voice_send_now(ROBOT_UART_CMD_TALK_END);
            } else {
                PR_NOTICE("robot voice: skip vet before mouse transform");
            }
        }
        quaddle_robot_bridge_flush_deferred_mouse_transform();
        break;
    default:
        break;
    }
}

void robot_uart_voice_poll(void)
{
    if (!s_listen_pending || quaddle_robot_bridge_mouse_exclusive()) {
        return;
    }

    s_listen_pending = false;
    __robot_uart_voice_clear_applied_mood();
    __robot_uart_voice_send_now(ROBOT_UART_CMD_LISTEN);
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

static size_t __robot_voice_find(const char *text, const char *phrase)
{
    size_t text_len;
    size_t phrase_len;
    size_t i;
    size_t j;

    if (!text || !phrase || phrase[0] == '\0') {
        return (size_t)-1;
    }

    text_len = strlen(text);
    phrase_len = strlen(phrase);
    if (phrase_len > text_len) {
        return (size_t)-1;
    }

    for (i = 0; i <= text_len - phrase_len; i++) {
        for (j = 0; j < phrase_len; j++) {
            if (__robot_voice_ascii_tolower(text[i + j]) != __robot_voice_ascii_tolower(phrase[j])) {
                break;
            }
        }
        if (j == phrase_len) {
            return i;
        }
    }

    return (size_t)-1;
}

typedef struct {
    const char         *phrase;
    ROBOT_EYE_MOOD_E    mood;
} ROBOT_NLG_MOOD_RULE_T;

static const ROBOT_NLG_MOOD_RULE_T s_nlg_mood_rules[] = {
    {"hahaha", ROBOT_EYE_MOOD_HAPPY},
    {"haha", ROBOT_EYE_MOOD_HAPPY},
    {"ha ha", ROBOT_EYE_MOOD_HAPPY},
    {"woohoo", ROBOT_EYE_MOOD_HAPPY},
    {"grinning", ROBOT_EYE_MOOD_HAPPY},
    {"smiling", ROBOT_EYE_MOOD_HAPPY},
    {"smile", ROBOT_EYE_MOOD_HAPPY},
    {"happy", ROBOT_EYE_MOOD_HAPPY},
    {"cute", ROBOT_EYE_MOOD_HAPPY},
    {"cheer", ROBOT_EYE_MOOD_HAPPY},
    {"lmao", ROBOT_EYE_MOOD_HAPPY},
    {"lol", ROBOT_EYE_MOOD_HAPPY},
    {"yay", ROBOT_EYE_MOOD_HAPPY},
    {"awesome", ROBOT_EYE_MOOD_HAPPY},
    {"funny", ROBOT_EYE_MOOD_HAPPY},
    {"glad", ROBOT_EYE_MOOD_HAPPY},
    {"\xF0\x9F\x98\x8A", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\x93\x88\xE5\x93\x88", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\x91\xB5\xE5\x91\xB5", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\x98\xBF\xE5\x98\xBF", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\x98\xBB\xE5\x98\xBB", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\xBC\x80\xE5\xBF\x83", ROBOT_EYE_MOOD_HAPPY},
    {"\xE9\xAB\x98\xE5\x85\xB4", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\xA4\xAA\xE5\xA5\xBD\xE4\xBA\x86", ROBOT_EYE_MOOD_HAPPY},
    {"\xE5\xA4\xAA\xE6\xa3\x92", ROBOT_EYE_MOOD_HAPPY},
    {"\xE7\x9C\x9F\xE6\xA3\x92", ROBOT_EYE_MOOD_HAPPY},
    {"unfortunately", ROBOT_EYE_MOOD_SAD},
    {"unhappy", ROBOT_EYE_MOOD_SAD},
    {"sorry", ROBOT_EYE_MOOD_SAD},
    {"sad", ROBOT_EYE_MOOD_SAD},
    {"pity", ROBOT_EYE_MOOD_SAD},
    {"\xE5\xAF\xB9\xE4\xB8\x8D\xE8\xB5\xB7", ROBOT_EYE_MOOD_SAD},
    {"\xE9\x9A\xBE\xE8\xBF\x87", ROBOT_EYE_MOOD_SAD},
    {"\xE4\xBC\xA4\xE5\xBF\x83", ROBOT_EYE_MOOD_SAD},
    {"\xE6\x8A\xB1\xE6\xad\x89", ROBOT_EYE_MOOD_SAD},
    {"\xE9\x81\x97\xE6\x86\xBE", ROBOT_EYE_MOOD_SAD},
    {"\xE5\x8F\xAF\xE6\x83\x9C", ROBOT_EYE_MOOD_SAD},
    {"\xE5\x91\x9C\xE5\x91\x9C", ROBOT_EYE_MOOD_SAD},
    {"not smiling", ROBOT_EYE_MOOD_SQUINT},
    {"furrowed", ROBOT_EYE_MOOD_SQUINT},
    {"fuming", ROBOT_EYE_MOOD_SQUINT},
    {"angry", ROBOT_EYE_MOOD_SQUINT},
    {"not sure", ROBOT_EYE_MOOD_SQUINT},
    {"really?", ROBOT_EYE_MOOD_SQUINT},
    {"perhaps", ROBOT_EYE_MOOD_SQUINT},
    {"maybe", ROBOT_EYE_MOOD_SQUINT},
    {"doubt", ROBOT_EYE_MOOD_SQUINT},
    {"confused", ROBOT_EYE_MOOD_SQUINT},
    {"hmm", ROBOT_EYE_MOOD_SQUINT},
    {"\xE4\xB8\x8D\xE7\xa1\xae\xE5\xae\x9a", ROBOT_EYE_MOOD_SQUINT},
    {"\xE8\xae\xa9\xE6\x88\x91\xE6\x83\xB3\xE6\x83\xB3", ROBOT_EYE_MOOD_SQUINT},
    {"\xE7\x9c\x9f\xE7\x9a\x84\xE5\x90\x97", ROBOT_EYE_MOOD_SQUINT},
    {"\xE6\x80\x80\xE7\x96\x91", ROBOT_EYE_MOOD_SQUINT},
    {"\xE5\xa5\x87\xE6\x80\xaa", ROBOT_EYE_MOOD_SQUINT},
    {"\xE6\x9c\xaa\xE5\xbf\x85", ROBOT_EYE_MOOD_SQUINT},
};

static ROBOT_EYE_MOOD_E __robot_uart_mood_from_text(const char *text)
{
    size_t i;
    size_t best_pos = (size_t)-1;
    ROBOT_EYE_MOOD_E best_mood = ROBOT_EYE_MOOD_NONE;

    if (!text || text[0] == '\0') {
        return ROBOT_EYE_MOOD_NONE;
    }

    for (i = 0; i < sizeof(s_nlg_mood_rules) / sizeof(s_nlg_mood_rules[0]); i++) {
        size_t pos = __robot_voice_find(text, s_nlg_mood_rules[i].phrase);
        if (pos == (size_t)-1) {
            continue;
        }
        if (pos < best_pos) {
            best_pos = pos;
            best_mood = s_nlg_mood_rules[i].mood;
        }
    }

    return best_mood;
}

static const char *const s_nlg_stance_no[] = {
    "cannot",
    "can't",
    "won't",
    "don't",
    "do not",
    "no way",
    "nope",
    "not able",
    "\xE4\xB8\x8D\xE5\x8F\xAF\xE4\xBB\xA5",
    "\xE5\x81\x9A\xE4\xB8\x8D\xE5\x88\xB0",
    "\xE6\xB2\xA1\xE6\xB3\x95",
    "\xE4\xB8\x8D\xE8\x83\xBD",
    "\xE4\xB8\x8D\xE8\xA1\x8C",
    "\xE4\xB8\x8D\xE8\xA6\x81",
    "\xE4\xB8\x8D\xE4\xBC\x9A",
    "\xE4\xB8\x8D\xE6\x98\xAF",
    "\xE6\x8B\x92\xE7\xBB\x9D",
};

static const char *const s_nlg_stance_yes[] = {
    "no problem",
    "of course",
    "sure",
    "yeah",
    "yes",
    "okay",
    "\xE6\xB2\xA1\xE9\x94\x99",
    "\xE5\xBD\x93\xE7\x84\xB6\xE5\x8F\xAF\xE4\xBB\xA5",
    "\xE6\xB2\xA1\xE9\x97\xAE\xE9\xA2\x98",
    "\xE5\x8F\xAF\xE4\xBB\xA5\xE7\x9A\x84",
    "\xE5\xA5\xBD\xE7\x9A\x84",
    "\xE6\x98\xAF\xE7\x9A\x84",
    "\xE5\xAF\xB9\xE7\x9A\x84",
    "\xE5\xAF\xB9\xE5\x95\x8A",
    "\xE5\xA5\xBD\xE5\x95\x8A",
    "\xE5\xA5\xBD\xE5\x91\x80",
    "\xE5\xBD\x93\xE7\x84\xB6",
};

static ROBOT_REPLY_STANCE_E __robot_uart_stance_from_text(const char *text)
{
    size_t i;

    if (!text || text[0] == '\0') {
        return ROBOT_REPLY_STANCE_NONE;
    }

    /* Affirmations such as 没错 must win before any 没/不 negative prefixes. */
    if (__robot_voice_contains(text, "\xE6\xB2\xA1\xE9\x94\x99") ||
        __robot_voice_contains(text, "\xE6\x98\xAF\xE7\x9A\x84") ||
        __robot_voice_contains(text, "\xE5\xAF\xB9\xE7\x9A\x84")) {
        return ROBOT_REPLY_STANCE_YES;
    }

    for (i = 0; i < sizeof(s_nlg_stance_no) / sizeof(s_nlg_stance_no[0]); i++) {
        if (__robot_voice_contains(text, s_nlg_stance_no[i])) {
            return ROBOT_REPLY_STANCE_NO;
        }
    }
    for (i = 0; i < sizeof(s_nlg_stance_yes) / sizeof(s_nlg_stance_yes[0]); i++) {
        if (__robot_voice_contains(text, s_nlg_stance_yes[i])) {
            return ROBOT_REPLY_STANCE_YES;
        }
    }

    return ROBOT_REPLY_STANCE_NONE;
}

static ROBOT_EYE_MOOD_E __robot_uart_mood_from_emoji_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return ROBOT_EYE_MOOD_NONE;
    }

    if (strcmp(name, EMOJI_HAPPY) == 0 || strcmp(name, EMOJI_LAUGHING) == 0 || strcmp(name, EMOJI_FUNNY) == 0 ||
        strcmp(name, EMOJI_LOVING) == 0 || strcmp(name, EMOJI_WINK) == 0 || strcmp(name, EMOJI_COOL) == 0 ||
        strcmp(name, EMOJI_RELAXED) == 0 || strcmp(name, EMOJI_DELICIOUS) == 0 || strcmp(name, EMOJI_KISSY) == 0 ||
        strcmp(name, EMOJI_CONFIDENT) == 0 || strcmp(name, EMOJI_SILLY) == 0) {
        return ROBOT_EYE_MOOD_HAPPY;
    }

    if (strcmp(name, EMOJI_SAD) == 0 || strcmp(name, EMOJI_FEARFUL) == 0 ||
        strcmp(name, EMOJI_DISAPPOINTED) == 0 || strcmp(name, EMOJI_EMBARRASSED) == 0) {
        return ROBOT_EYE_MOOD_SAD;
    }

    if (strcmp(name, EMOJI_THINKING) == 0 || strcmp(name, EMOJI_CONFUSED) == 0 || strcmp(name, EMOJI_ANGRY) == 0 ||
        strcmp(name, EMOJI_ANNOYED) == 0 || strcmp(name, EMOJI_SURPRISE) == 0 || strcmp(name, EMOJI_SHOCKED) == 0) {
        return ROBOT_EYE_MOOD_SQUINT;
    }

    return ROBOT_EYE_MOOD_NONE;
}

static int __robot_uart_hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static uint32_t __robot_uart_parse_emoji_code(const char *emoji)
{
    uint32_t code = 0;
    const char *p;
    int digit;

    if (!emoji || emoji[0] == '\0') {
        return 0;
    }
    p = emoji;
    if ((p[0] == 'U' || p[0] == 'u') && p[1] == '+') {
        p += 2;
    }
    while (*p) {
        digit = __robot_uart_hex_val(*p++);
        if (digit < 0) {
            return 0;
        }
        code = (code << 4) | (uint32_t)digit;
    }
    return code;
}

static ROBOT_EYE_MOOD_E __robot_uart_mood_from_emoji_code(uint32_t code)
{
    switch (code) {
    case 0x1F60A:
    case 0x1F600:
    case 0x1F601:
    case 0x1F603:
    case 0x1F604:
    case 0x1F605:
    case 0x1F642:
    case 0x1F606:
    case 0x1F602:
    case 0x1F60D:
    case 0x1F609:
    case 0x1F60E:
    case 0x1F60C:
    case 0x1F618:
    case 0x1F60F:
    case 0x1F61C:
    case 0x1F970:
    case 0x1F917:
        return ROBOT_EYE_MOOD_HAPPY;
    case 0x1F614:
    case 0x1F62D:
    case 0x1F622:
    case 0x1F61E:
    case 0x1F641:
        return ROBOT_EYE_MOOD_SAD;
    case 0x1F620:
    case 0x1F621:
    case 0x1F914:
    case 0x1F644:
    case 0x1F62F:
    case 0x1F631:
        return ROBOT_EYE_MOOD_SQUINT;
    default:
        return ROBOT_EYE_MOOD_NONE;
    }
}

static const char *__robot_uart_talk_cmd_for_mood(ROBOT_EYE_MOOD_E mood)
{
    switch (mood) {
    case ROBOT_EYE_MOOD_HAPPY:
        return ROBOT_UART_CMD_TALK_HAPPY;
    case ROBOT_EYE_MOOD_SAD:
        return ROBOT_UART_CMD_TALK_SAD;
    case ROBOT_EYE_MOOD_SQUINT:
        return ROBOT_UART_CMD_TALK_SQUINT;
    default:
        return ROBOT_UART_CMD_TALK;
    }
}

static const char *__robot_uart_mood_cmd(ROBOT_EYE_MOOD_E mood)
{
    switch (mood) {
    case ROBOT_EYE_MOOD_HAPPY:
        return ROBOT_UART_CMD_MOOD_HAPPY;
    case ROBOT_EYE_MOOD_SAD:
        return ROBOT_UART_CMD_MOOD_SAD;
    case ROBOT_EYE_MOOD_SQUINT:
        return ROBOT_UART_CMD_MOOD_SQUINT;
    default:
        return NULL;
    }
}

static void __robot_uart_voice_reset_reply_mood(void)
{
    s_reply_mood = ROBOT_EYE_MOOD_NONE;
    s_nlg_mood_len = 0;
    s_nlg_mood_buf[0] = '\0';
    s_reply_stance = ROBOT_REPLY_STANCE_NONE;
}

static void __robot_uart_voice_append_nlg(const char *data, uint16_t datalen)
{
    size_t copy_len;

    if (!data || datalen == 0) {
        return;
    }

    if (s_nlg_mood_len >= sizeof(s_nlg_mood_buf) - 1) {
        return;
    }

    copy_len = datalen;
    if (copy_len > sizeof(s_nlg_mood_buf) - 1 - s_nlg_mood_len) {
        copy_len = sizeof(s_nlg_mood_buf) - 1 - s_nlg_mood_len;
    }
    memcpy(s_nlg_mood_buf + s_nlg_mood_len, data, copy_len);
    s_nlg_mood_len += copy_len;
    s_nlg_mood_buf[s_nlg_mood_len] = '\0';
}

static void __robot_uart_voice_set_reply_mood(ROBOT_EYE_MOOD_E mood, const char *source)
{
    if (mood == ROBOT_EYE_MOOD_NONE) {
        return;
    }

    if (s_reply_mood != mood) {
        PR_NOTICE("robot voice: reply mood %d from %s", (int)mood, source ? source : "?");
    }
    s_reply_mood = mood;
    if (s_talking && s_applied_mood != mood) {
        __robot_uart_voice_send_mood_only(mood);
    }
}

static void __robot_uart_voice_on_text_stream(const AI_NOTIFY_TEXT_T *text, bool is_start)
{
    ROBOT_EYE_MOOD_E mood;

    if (is_start) {
        s_nlg_mood_len = 0;
        s_nlg_mood_buf[0] = '\0';
        s_reply_stance = ROBOT_REPLY_STANCE_NONE;
    }
    if (!text || !text->data || text->datalen == 0) {
        return;
    }

    __robot_uart_voice_append_nlg(text->data, text->datalen);
    mood = __robot_uart_mood_from_text(s_nlg_mood_buf);
    __robot_uart_voice_set_reply_mood(mood, "nlg");
    s_reply_stance = __robot_uart_stance_from_text(s_nlg_mood_buf);
    if (s_talking) {
        __robot_uart_voice_queue_reply_stance();
    }
}

static void __robot_uart_voice_clear_applied_mood(void)
{
    if (s_applied_mood == ROBOT_EYE_MOOD_NONE) {
        return;
    }
    __robot_uart_voice_send_now(ROBOT_UART_CMD_EFFECT_END);
    s_applied_mood = ROBOT_EYE_MOOD_NONE;
}

static void __robot_uart_voice_send_mood_only(ROBOT_EYE_MOOD_E mood)
{
    const char *cmd = __robot_uart_mood_cmd(mood);

    if (!cmd) {
        return;
    }
    if (s_applied_mood != ROBOT_EYE_MOOD_NONE && s_applied_mood != mood) {
        __robot_uart_voice_clear_applied_mood();
    }
    __robot_uart_voice_send_now(cmd);
    s_applied_mood = mood;
}

static void __robot_uart_voice_on_emotion(const AI_NOTIFY_EMO_T *emo)
{
    ROBOT_EYE_MOOD_E mood;

    if (!emo) {
        return;
    }

    mood = __robot_uart_mood_from_emoji_name(emo->name);
    if (mood == ROBOT_EYE_MOOD_NONE) {
        mood = __robot_uart_mood_from_emoji_code(__robot_uart_parse_emoji_code(emo->emoji));
    }
    if (mood == ROBOT_EYE_MOOD_NONE && emo->emoji &&
        (__robot_uart_parse_emoji_code(emo->emoji) == 0x1F636)) {
        s_reply_mood = ROBOT_EYE_MOOD_NONE;
        PR_NOTICE("robot voice: reply mood cleared by explicit neutral tag");
        return;
    }

    __robot_uart_voice_set_reply_mood(mood, "emotion");
}

static void __robot_uart_voice_send_talk_with_mood(void)
{
    if (s_applied_mood != ROBOT_EYE_MOOD_NONE && s_applied_mood != s_reply_mood) {
        __robot_uart_voice_clear_applied_mood();
    }

    __robot_uart_voice_send_now(__robot_uart_talk_cmd_for_mood(s_reply_mood));
    s_applied_mood = s_reply_mood;
}

static void __robot_uart_voice_queue_reply_stance(void)
{
    const char *cmd = NULL;
    OPERATE_RET rt;

    if (s_stance_sent) {
        return;
    }
    if (s_reply_stance == ROBOT_REPLY_STANCE_YES) {
        cmd = "knd";
    } else if (s_reply_stance == ROBOT_REPLY_STANCE_NO) {
        cmd = "kwh";
    } else {
        return;
    }

    rt = quaddle_robot_bridge_queue_ai_command_front(cmd, "NLG");
    if (rt != OPRT_OK) {
        PR_ERR("robot voice: queue stance \"%s\" failed %d", cmd, rt);
        return;
    }

    s_stance_sent = true;
    PR_NOTICE("robot voice: reply stance %s -> UART \"%s\"",
              s_reply_stance == ROBOT_REPLY_STANCE_YES ? "yes" : "no", cmd);
}

/* Longer phrases first to avoid partial false matches */
static const ROBOT_VOICE_RULE_T s_voice_rules[] = {
    {"\xE6\x97\x8B\xE8\xBD\xAC\xE4\xB8\x80\xE5\x9C\x88", "kvtL 360"},
    {"\xE8\xBD\xAC\xE4\xB8\x80\xE5\x9C\x88", "kvtL 360"},
    {"turn around", "kvtL 360"},
    {"spin around", "kvtL 360"},
    {"do a spin", "kvtL 360"},
    {"rotate", "kvtL 360"},
    {"spin", "kvtL 360"},
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
    {"\xE8\xB7\xB3\xE6\xBB\x91", "kjumpSlide"},
    {"jump slide", "kjumpSlide"},
    {"\xE5\x8F\x98\xE6\x88\x90\xE8\x80\x81\xE9\xBC\xA0", "XO"},
    {"\xE5\x8F\x98\xE6\x88\x90\xE9\xBC\xA0\xE6\xA0\x87", "XO"},
    {"become a mouse", "XO"},
    {"turn into a mouse", "XO"},
    {"\xE5\x8F\x98\xE5\x9B\x9E\xE6\x9D\xA5", "Xo"},
    {"\xE5\x8F\x98\xE5\x9B\x9E", "Xo"},
    {"return to normal", "Xo"},
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
        {"\xE7\xB4\xAB\xE8\x89\xB2", "vcv"},
        {"orange", "vco"},
        {"yellow", "vcy"},
        {"purple", "vcv"},
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
        {"\xE7\xB4\xAB", "vcv"},
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

    /* Let the cloud interpret negation and complaints such as "you're not spinning". */
    if ((__robot_voice_contains(asr, "spin") || __robot_voice_contains(asr, "rotate")) &&
        (__robot_voice_contains(asr, "don't") || __robot_voice_contains(asr, "do not") ||
         __robot_voice_contains(asr, "not spin") || __robot_voice_contains(asr, "stop spin"))) {
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

    if (strncmp(cmd, "knd", 3) == 0 || strncmp(cmd, "kwh", 3) == 0) {
        s_explicit_head_gesture = true;
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
    ROBOT_EYE_MOOD_E mood;

    if (text && text[0] != '\0') {
        s_expected_reply_language = __robot_voice_detect_language(text);
        PR_NOTICE("robot voice: expected NLG language %s from latest ASR",
                  s_expected_reply_language == ROBOT_REPLY_LANGUAGE_CHINESE ? "Chinese" :
                  s_expected_reply_language == ROBOT_REPLY_LANGUAGE_ENGLISH ? "English" : "unknown");
        __robot_uart_voice_reset_reply_mood();
        mood = __robot_uart_mood_from_text(text);
        __robot_uart_voice_set_reply_mood(mood, "asr");
    }

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

bool robot_uart_voice_allow_mcp_head_gesture(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return true;
    }
    if (strncmp(cmd, "knd", 3) != 0 && strncmp(cmd, "kwh", 3) != 0) {
        return true;
    }
    return s_explicit_head_gesture;
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

    if (text && text[0] != '\0' && __robot_voice_nlg_language_mismatch(text)) {
        PR_NOTICE("robot voice: NLG language differs from latest ASR; still play TTS");
    }

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

    PR_NOTICE("robot voice: ASR->UART1 enabled; record/talk cues vetL, velT/veTH/veTS/veTP, vet; CLI: robot_uart [cmd]");
    return OPRT_OK;
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
