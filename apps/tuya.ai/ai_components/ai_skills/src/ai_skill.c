/**
 * @file ai_skill.c
 * @brief AI skill module implementation
 *
 * This module implements AI skill processing, including emotion skills,
 * music/story skills, and play control skills.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */
#include "tal_api.h"
#include "cJSON.h"
#include "mix_method.h"

#include "ai_user_event.h"
#include "skill_emotion.h"

__attribute__((weak)) void ai_app_on_asr_result(const char *text)
{
    (void)text;
}

__attribute__((weak)) void ai_app_filter_nlg_text(char *text, bool eof)
{
    (void)text;
    (void)eof;
}

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
#include "ai_audio_player.h"
#include "skill_music_story.h"
#endif

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
#include "ai_picture_output.h"
#endif

#include "ai_skill.h"

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
static int __ai_hex_to_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

static bool __ai_parse_unicode_escape(const char *text, size_t remaining, unsigned int *codepoint)
{
    unsigned int value = 0;
    int          digit = 0;
    size_t       i     = 0;

    if (remaining < 6 || text[0] != '\\' || text[1] != 'u') {
        return false;
    }

    for (i = 2; i < 6; i++) {
        digit = __ai_hex_to_value(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (unsigned int)digit;
    }

    *codepoint = value;
    return true;
}

/*
 * Some cloud responses contain a second JSON-escaped layer, for example
 * "\\u0020hello" after cJSON has already parsed the outer JSON object.
 * Decode that remaining Unicode escape layer in place. UTF-8 output is never
 * longer than the corresponding escape, so no additional buffer is needed.
 */
static bool __ai_decode_literal_unicode_escapes(char *text)
{
    size_t       read_pos  = 0;
    size_t       write_pos = 0;
    size_t       text_len  = strlen(text);
    bool         decoded   = false;
    unsigned int codepoint = 0;

    while (read_pos < text_len) {
        size_t escape_len = 6;

        if (!__ai_parse_unicode_escape(text + read_pos, text_len - read_pos, &codepoint) || codepoint == 0 ||
            (codepoint >= 0xDC00 && codepoint <= 0xDFFF)) {
            text[write_pos++] = text[read_pos++];
            continue;
        }

        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            unsigned int low_surrogate = 0;

            if (!__ai_parse_unicode_escape(text + read_pos + 6, text_len - read_pos - 6, &low_surrogate) ||
                low_surrogate < 0xDC00 || low_surrogate > 0xDFFF) {
                text[write_pos++] = text[read_pos++];
                continue;
            }

            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low_surrogate - 0xDC00);
            escape_len = 12;
        }

        if (codepoint <= 0x7F) {
            text[write_pos++] = (char)codepoint;
        } else if (codepoint <= 0x7FF) {
            text[write_pos++] = (char)(0xC0 | (codepoint >> 6));
            text[write_pos++] = (char)(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            text[write_pos++] = (char)(0xE0 | (codepoint >> 12));
            text[write_pos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            text[write_pos++] = (char)(0x80 | (codepoint & 0x3F));
        } else {
            text[write_pos++] = (char)(0xF0 | (codepoint >> 18));
            text[write_pos++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            text[write_pos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            text[write_pos++] = (char)(0x80 | (codepoint & 0x3F));
        }

        read_pos += escape_len;
        decoded = true;
    }

    text[write_pos] = '\0';
    return decoded;
}

/**
 * @brief Process AI skill data from JSON.
 *
 * @param root JSON root object containing skill data.
 * @param eof End of file flag indicating if this is the last data chunk.
 * @return OPERATE_RET Operation result code.
 */
static OPERATE_RET __ai_skills_process(cJSON *root, bool eof)
{
    OPERATE_RET  rt   = OPRT_OK;
    const cJSON *node = NULL;
    const char  *code = NULL;

    /* Root is data:{}, parse code */
    node = cJSON_GetObjectItem(root, "code");
    code = cJSON_GetStringValue(node);
    if (!code)
        return OPRT_OK;

    PR_NOTICE("text -> skill code: %s", code);
    if (strcmp(code, "emo") == 0 || strcmp(code, "llm_emo") == 0) {
        ai_skill_emo_process(root);
    }
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    else if (strcmp(code, "music") == 0 || strcmp(code, "story") == 0) {
        AI_AUDIO_MUSIC_T *music = NULL;
        if (ai_skill_parse_music(root, &music) == OPRT_OK) {
            ai_skill_parse_music_dump(music);
            ai_audio_play_music(music);
            ai_skill_parse_music_free(music);
        }
    } else if (strcmp(code, "PlayControl") == 0) {
        AI_AUDIO_MUSIC_T *music = NULL;
        if ((rt = ai_skill_parse_playcontrol(root, &music)) == 0) {
            ai_skill_parse_music_dump(music);
            ai_skill_playcontrol_music(music);
            ai_skill_parse_music_free(music);
        }
    }
#endif
    else {
        PR_NOTICE("skill %s not handled", code);
        /* PR_NOTICE("skill content %s ", cJSON_PrintUnformatted(root)); */

        ai_user_event_notify(AI_USER_EVT_SKILL, root);
    }

    return rt;
}

/**
 * @brief Process ASR (Automatic Speech Recognition) text stream.
 *
 * @param root JSON root object containing ASR data.
 * @param eof End of file flag indicating if this is the last data chunk.
 * @return OPERATE_RET Operation result code.
 */
static OPERATE_RET __ai_asr_process(cJSON *root, bool eof)
{
    char *content = cJSON_GetStringValue(root);
    PR_NOTICE("text -> ASR result: %s", content);
    if (!content) {
        content = "";
    }

    if (content[0] != '\0') {
        ai_app_on_asr_result(content);
    }

    AI_NOTIFY_TEXT_T text;
    text.data      = content;
    text.datalen   = strlen(content);
    text.timeindex = 0;
    ai_user_event_notify((0 == strlen(content)) ? AI_USER_EVT_ASR_EMPTY : AI_USER_EVT_ASR_OK, &text);

    return OPRT_OK;
}

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
/**
 * @brief Process image URLs from JSON and start picture output.
 *
 * @param root JSON root object containing images data.
 * @return OPERATE_RET Operation result code.
 */
static OPERATE_RET __ai_images_process(cJSON *root)
{
    cJSON *images = cJSON_GetObjectItem(root, "images");
    if (NULL == images) {
        PR_ERR("no images found");
        return OPRT_COM_ERROR;
    }

    cJSON *url_array = cJSON_GetObjectItem(images, "url");
    if (NULL == url_array || !cJSON_IsArray(url_array)) {
        PR_ERR("no url array found");
        return OPRT_COM_ERROR;
    }

    int url_count = cJSON_GetArraySize(url_array);
    for (int i = 0; i < url_count; i++) {
        cJSON *url_item = cJSON_GetArrayItem(url_array, i);
        if (NULL == url_item) {
            PR_ERR("url item is null");
            continue;
        }

        char *url_str = cJSON_GetStringValue(url_item);
        if (NULL == url_str) {
            PR_ERR("url string is null");
            continue;
        }

        PR_NOTICE("image url[%d]: %s", i, url_str);

        /* #define TEST_IMG_URL "https://images.tuyacn.com/fe-static/docs/img/bef36953-4002-4a7c-b567-db05a6c5e2cd.jpeg"
         */

        /* ai_picture_output_start(TEST_IMG_URL); */
        ai_picture_output_start(url_str);
    }

    return OPRT_OK;
}
#endif

/**
 * @brief Process NLG (Natural Language Generation) text stream.
 *
 * @param root JSON root object containing NLG data.
 * @param eof End of file flag indicating if this is the last data chunk.
 * @return OPERATE_RET Operation result code.
 */
static OPERATE_RET __ai_nlg_process(cJSON *root, bool eof)
{
    char *json_str = cJSON_PrintUnformatted(root);
    PR_NOTICE("json-str %s", json_str);
    cJSON_free(json_str);

    cJSON *nlgResult = cJSON_GetObjectItem(root, "nlgResult");
    if (nlgResult) {
#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
        if (ai_picture_is_init() == true) {
            if (__ai_images_process(nlgResult) != OPRT_OK) {
                PR_NOTICE("process nlg images failed");
            } else {
                PR_NOTICE("process nlg images success");
            }
        }
#endif
        return OPRT_OK;
    }

    char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    if (!content) {
        content = "";
    } else if (__ai_decode_literal_unicode_escapes(content)) {
        PR_DEBUG("decoded literal Unicode escapes in NLG content");
    }

    ai_app_filter_nlg_text(content, eof);

    AI_NOTIFY_TEXT_T text = {0};
    cJSON           *time_index = cJSON_GetObjectItem(root, "timeIndex");

    text.data    = content;
    text.datalen = strlen(content);
    if (cJSON_IsNumber(time_index) && time_index->valuedouble >= 0) {
        text.timeindex = (uint32_t)time_index->valuedouble;
    }
    PR_NOTICE("text -> NLG eof: %d, content: %s, time: %u", eof, content, (unsigned int)text.timeindex);

    /* Send data to register callback */
    static AI_USER_EVT_TYPE_E event_type = AI_USER_EVT_TEXT_STREAM_STOP;
    if (event_type == AI_USER_EVT_TEXT_STREAM_STOP) {
        if (eof) {
            if (strlen(content) > 0) {
                ai_user_event_notify(AI_USER_EVT_TEXT_STREAM_START, &text);
                text.data    = NULL;
                text.datalen = 0;
                ai_user_event_notify(AI_USER_EVT_TEXT_STREAM_STOP, &text);
                event_type = AI_USER_EVT_TEXT_STREAM_STOP;
            }
        } else {
            ai_user_event_notify(AI_USER_EVT_TEXT_STREAM_START, &text);
            event_type = AI_USER_EVT_TEXT_STREAM_DATA;
        }
    } else {
        if (event_type == AI_USER_EVT_TEXT_STREAM_DATA) {
            ai_user_event_notify(eof ? AI_USER_EVT_TEXT_STREAM_STOP : AI_USER_EVT_TEXT_STREAM_DATA, &text);
            event_type = eof ? AI_USER_EVT_TEXT_STREAM_STOP : AI_USER_EVT_TEXT_STREAM_DATA;
        }
    }

    AI_AGENT_EMO_T emo;
    cJSON         *tags_array = cJSON_GetObjectItem(root, "tags");
    if (tags_array && cJSON_IsArray(tags_array) && cJSON_GetArraySize(tags_array) > 0) {
        char *emoji = cJSON_GetStringValue(cJSON_GetArrayItem(tags_array, 0));
        if (emoji && strlen(emoji)) {
            emo.emoji = emoji;
            emo.name  = ai_agent_emoji_get_name(emoji);
            ai_agent_play_emo(&emo);
        }
    }

    return OPRT_OK;
}

/**
 * @brief Process AI text data based on type.
 *
 * @param type Text type (ASR, NLG, SKILL, CLOUD_EVENT).
 * @param root JSON root object containing text data.
 * @param eof End of file flag indicating if this is the last data chunk.
 * @return OPERATE_RET Operation result code.
 */
OPERATE_RET ai_text_process(AI_TEXT_TYPE_E type, cJSON *root, bool eof)
{
    TUYA_CHECK_NULL_RETURN(root, OPRT_INVALID_PARM);

    switch (type) {
    case AI_TEXT_ASR:
        __ai_asr_process(root, eof);
        break;
    case AI_TEXT_NLG:
        __ai_nlg_process(root, eof);
        break;
    case AI_TEXT_SKILL:
        __ai_skills_process(root, eof);
        break;
    case AI_TEXT_CLOUD_EVENT:
        ai_parse_cloud_event(root);
        break;
    default:
        /* PR_NOTICE("ai agent -> unknown text type: %d", type); */
        /* char *content = cJSON_PrintUnformatted(root); */
        /* PR_NOTICE("text content: %s", content); */
        /* cJSON_free(content); */
        break;
    }

    return OPRT_OK;
}
