/**
 * @file ai_mode_free.c
 * @version 0.1
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tuya_cloud_types.h"

#if defined(ENABLE_COMP_AI_MODE_FREE) && (ENABLE_COMP_AI_MODE_FREE == 1)

#include "tal_api.h"
#include "tuya_ai_agent.h"

#include "tkl_vad.h"
#include "tkl_kws.h"

#if defined(ENABLE_LED) && (ENABLE_LED == 1)
#include "tdl_led_manage.h"
#endif

#include "lang_config.h"
#include "ai_user_event.h"

#include "ai_audio_input.h"
#include "ai_audio_player.h"
#include "ai_agent.h"
#include "ai_manage_mode.h"
#include "ai_mode_free.h"
#include "tuya_ai_input.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define MODE_STATE_CHANGE(_old, _new) \
do { \
    PR_DEBUG("mode free state change form %s to %s", ai_get_mode_state_str(_old),\
                                                     ai_get_mode_state_str(_new));\
    _old = _new; \
}while (0)

#define AI_CHAT_LISTEN_TIMEOUT_MS    (10 * 1000)    // 10sec
#define AI_CHAT_RESPONSE_TIMEOUT_MS (30 * 1000)     // 30sec
#define AI_CHAT_LISTEN_ARM_DELAY_MS 250
#define AI_CHAT_POST_PLAY_ARM_DELAY_MS 20
#define AI_CHAT_LISTEN_ARM_RETRY_MS 100
#define AI_CHAT_LISTEN_ARM_LOG_RETRIES 10
#define AI_CHAT_EMPTY_IDLE_LIMIT    3

/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
***********************variable define**********************
***********************************************************/
#if defined(ENABLE_LED) && (ENABLE_LED == 1)
static TDL_LED_HANDLE_T sg_led_hdl = NULL;
#endif

static AI_MODE_STATE_E sg_mode_set_state = AI_MODE_STATE_INIT;
static AI_MODE_STATE_E sg_mode_cur_state = AI_MODE_STATE_INVALID;
static bool            sg_is_wakeup = false;
static bool            sg_pending_vad_start = false;
static bool            sg_exit_pending = false;
static bool            sg_goodbye_pending = false;
static bool            sg_goodbye_duplicate_suppressed = false;
static bool            sg_goodbye_nlg_time_valid = false;
static uint8_t         sg_empty_asr_count = 0;
static uint8_t         sg_listen_arm_retries = 0;
static uint32_t        sg_listen_armed_ms = 0;
static uint32_t        sg_goodbye_nlg_last_timeindex = 0;
static TIMER_ID        sg_enter_idle_timer = NULL;
static TIMER_ID        sg_listen_arm_timer = NULL;
static uint32_t        sg_listen_timeout_ms = AI_CHAT_LISTEN_TIMEOUT_MS;
static uint32_t        sg_response_timeout_ms = AI_CHAT_RESPONSE_TIMEOUT_MS;
static char            sg_exit_prompt_zh[] =
    "请只回复下面这句话，不要添加任何其他内容:好的，如果你没有什么想聊的话题或者要求，我们下次再聊。";
static char sg_exit_prompt_en[] =
    "Reply in English with exactly this sentence and nothing else:Okay, if there is nothing else you would like to "
    "talk about or ask, let's chat again next time.";
#if defined(ENABLE_AI_LANGUAGE_ENGLISH) && (ENABLE_AI_LANGUAGE_ENGLISH == 1)
static bool sg_use_english_exit_prompt = true;
#else
static bool sg_use_english_exit_prompt = false;
#endif

/***********************************************************
***********************function define**********************
***********************************************************/
static void __ai_mode_free_stop_active_input(void)
{
    ai_audio_input_output_set(false);

    AI_INPUT_STATE_E input_state = tuya_ai_input_get_state();
    if (input_state == AI_INPUT_PROC) {
        PR_NOTICE("mode free stop active ai input");
        tuya_ai_input_stop();
    }
}

static void __ai_mode_free_update_language(const AI_NOTIFY_TEXT_T *text)
{
    bool has_ascii_letter = false;
    bool has_cjk = false;
    uint16_t i;

    if (!text || !text->data || text->datalen == 0) {
        return;
    }

    for (i = 0; i < text->datalen; i++) {
        const uint8_t ch = (uint8_t)text->data[i];

        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            has_ascii_letter = true;
            continue;
        }

        /* Chinese characters U+3400..U+9FFF encoded as three-byte UTF-8. */
        if (i + 2 < text->datalen && ch >= 0xE3 && ch <= 0xE9) {
            const uint8_t ch2 = (uint8_t)text->data[i + 1];
            const uint8_t ch3 = (uint8_t)text->data[i + 2];
            uint32_t codepoint;

            if ((ch2 & 0xC0) == 0x80 && (ch3 & 0xC0) == 0x80) {
                codepoint = ((uint32_t)(ch & 0x0F) << 12) | ((uint32_t)(ch2 & 0x3F) << 6) | (ch3 & 0x3F);
                if (codepoint >= 0x3400 && codepoint <= 0x9FFF) {
                    has_cjk = true;
                    break;
                }
                i += 2;
            }
        }
    }

    if (has_cjk) {
        sg_use_english_exit_prompt = false;
        PR_DEBUG("mode free language context: Chinese");
    } else if (has_ascii_letter) {
        sg_use_english_exit_prompt = true;
        PR_DEBUG("mode free language context: English");
    }
}

static void __ai_mode_free_complete_exit(void)
{
    bool duplicate_suppressed;

    if (!sg_goodbye_pending) {
        PR_DEBUG("mode free exit already completed");
        return;
    }

    /* Close the guard before any external call which may synchronously emit
     * PLAY_END and re-enter this function. */
    sg_goodbye_pending = false;
    duplicate_suppressed = sg_goodbye_duplicate_suppressed;
    sg_goodbye_duplicate_suppressed = false;

    if (duplicate_suppressed) {
        /* The buffered first sentence has finished. It is now safe to stop
         * the duplicate cloud response without cutting the audible goodbye. */
        tuya_ai_agent_event(AI_EVENT_CHAT_BREAK, 0);
    }

    sg_is_wakeup = false;
    sg_pending_vad_start = false;
    sg_exit_pending = false;
    sg_goodbye_nlg_time_valid = false;
    sg_goodbye_nlg_last_timeindex = 0;
    sg_empty_asr_count = 0;
    ai_audio_input_output_set(false);
    ai_app_on_free_mode_exit();
    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
}

static void __ai_mode_free_begin_exit(void)
{
    OPERATE_RET rt;

    if (sg_goodbye_pending || sg_mode_set_state == AI_MODE_STATE_IDLE) {
        return;
    }

    PR_NOTICE("mode free announce exit before idle");
    tal_sw_timer_stop(sg_enter_idle_timer);
    if (sg_listen_arm_timer) {
        tal_sw_timer_stop(sg_listen_arm_timer);
    }
    __ai_mode_free_stop_active_input();
    ai_audio_input_wakeup_set(false);
    sg_is_wakeup = false;
    sg_pending_vad_start = false;
    sg_exit_pending = false;
    sg_goodbye_pending = true;
    sg_goodbye_duplicate_suppressed = false;
    sg_goodbye_nlg_time_valid = false;
    sg_goodbye_nlg_last_timeindex = 0;
    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_THINK);

    rt = ai_agent_send_text(sg_use_english_exit_prompt ? sg_exit_prompt_en : sg_exit_prompt_zh);
    if (rt != OPRT_OK) {
        PR_WARN("mode free exit announcement request failed: %d", rt);
        __ai_mode_free_complete_exit();
        return;
    }

    /* Protect against a cloud request which never produces TTS. */
    tal_sw_timer_start(sg_enter_idle_timer, sg_response_timeout_ms, TAL_TIMER_ONCE);
}

static bool __ai_mode_free_is_goodbye_prefix(const AI_NOTIFY_TEXT_T *text)
{
    const char *prefix = sg_use_english_exit_prompt ? "Okay" : "好的";
    size_t      prefix_len = strlen(prefix);
    size_t      offset = 0;

    if (!text || !text->data) {
        return false;
    }

    while (offset < text->datalen &&
           (text->data[offset] == ' ' || text->data[offset] == '\t' || text->data[offset] == '\r' ||
            text->data[offset] == '\n')) {
        offset++;
    }

    return text->datalen - offset >= prefix_len && memcmp(text->data + offset, prefix, prefix_len) == 0;
}

static bool __ai_mode_free_suppress_duplicate_goodbye(const AI_NOTIFY_TEXT_T *text)
{
    if (!sg_goodbye_pending || !text || !text->data || text->datalen == 0) {
        return false;
    }

    /*
     * The cloud agent may produce a preliminary answer and then start a
     * second final-answer phase in the same request. Its NLG timeIndex resets
     * at that boundary. For the fixed goodbye prompt both phases can contain
     * the same sentence, which otherwise makes the TTS stream speak twice.
     */
    if (!sg_goodbye_duplicate_suppressed && text->timeindex > 0 && sg_goodbye_nlg_time_valid &&
        text->timeindex < sg_goodbye_nlg_last_timeindex && __ai_mode_free_is_goodbye_prefix(text)) {
        PR_WARN("mode free suppress duplicate goodbye phase, timeIndex %u -> %u",
                (unsigned int)sg_goodbye_nlg_last_timeindex, (unsigned int)text->timeindex);
        sg_goodbye_duplicate_suppressed = true;
        if (ai_audio_player_finish_tts_stream() != OPRT_OK) {
            PR_WARN("mode free failed to finish buffered goodbye audio");
            ai_audio_player_stop(AI_AUDIO_PLAYER_FG);
        }
        return true;
    }

    if (text->timeindex > 0) {
        sg_goodbye_nlg_last_timeindex = text->timeindex;
        sg_goodbye_nlg_time_valid = true;
    }

    return false;
}

static void __ai_mode_kws_wakeup(TKL_KWS_WAKEUP_WORD_E wakeup_word)
{
    tkl_kws_disable();
    ai_audio_player_stop(AI_AUDIO_PLAYER_ALL);
    ai_audio_input_output_set(false);
    ai_audio_input_wakeup_set(false);
    ai_audio_input_reset();
    tuya_ai_agent_event(AI_EVENT_CHAT_BREAK, 0);

    ai_audio_player_alert(AI_AUDIO_ALERT_WAKEUP);

    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
    sg_is_wakeup = true;
    sg_pending_vad_start = false;
    sg_exit_pending = false;
    sg_goodbye_pending = false;
    sg_empty_asr_count = 0;
}

static void __ai_mode_free_schedule_listen_arm(uint32_t delay_ms)
{
    ai_audio_input_output_set(false);
    ai_audio_input_wakeup_set(false);
    sg_pending_vad_start = false;
    sg_listen_arm_retries = 0;

    if (sg_listen_arm_timer) {
        tal_sw_timer_stop(sg_listen_arm_timer);
        tal_sw_timer_start(sg_listen_arm_timer, delay_ms, TAL_TIMER_ONCE);
    }
    PR_DEBUG("mode free listen arm scheduled in %d ms", delay_ms);
}

static void __ai_mode_free_start_input(void)
{
    if (!sg_is_wakeup || sg_exit_pending || sg_mode_set_state != AI_MODE_STATE_LISTEN) {
        return;
    }

    if (ai_audio_player_is_playing()) {
        sg_pending_vad_start = true;
        PR_DEBUG("[====ai_free] delay vad start while player is playing");
        return;
    }

    AI_INPUT_STATE_E input_state = tuya_ai_input_get_state();
    if (input_state != AI_INPUT_IDLE) {
        PR_DEBUG("[====ai_free] ignore vad start, ai input state:%d", input_state);
        return;
    }

    sg_pending_vad_start = false;
    tuya_ai_agent_set_scode(AI_AGENT_SCODE_DEFAULT);
    tuya_ai_input_start(false);
    ai_audio_input_output_set(true);
    ai_app_on_record_start();
}

static void __ai_mode_enter_idle(void)
{
#if defined(ENABLE_LED) && (ENABLE_LED == 1)   
    tdl_led_set_status(sg_led_hdl, TDL_LED_OFF);
#endif

    tal_sw_timer_stop(sg_enter_idle_timer);
    if (sg_listen_arm_timer) {
        tal_sw_timer_stop(sg_listen_arm_timer);
    }

    //disable wakeup
    __ai_mode_free_stop_active_input();
    ai_audio_input_wakeup_set(false);
    tkl_vad_start();
    tkl_kws_enable();

    sg_is_wakeup = false;
    sg_pending_vad_start = false;
    sg_exit_pending = false;
    sg_goodbye_pending = false;
    sg_empty_asr_count = 0;

    tkl_vad_set_threshold(TKL_AUDIO_VAD_LOW);
}

static void __ai_mode_enter_listen(void)
{
#if defined(ENABLE_LED) && (ENABLE_LED == 1)   
    tdl_led_flash(sg_led_hdl, 500);
#endif

    tal_sw_timer_start(sg_enter_idle_timer, sg_listen_timeout_ms, TAL_TIMER_ONCE);

    sg_is_wakeup = true;
    __ai_mode_free_schedule_listen_arm(AI_CHAT_LISTEN_ARM_DELAY_MS);
}

static void __ai_mode_enter_upload(void)
{
    PR_DEBUG("[====ai_free] upload");
}

static void __ai_mode_enter_think(void)
{
#if defined(ENABLE_LED) && (ENABLE_LED == 1)   
    tdl_led_flash(sg_led_hdl, 2000);
#endif

    tal_sw_timer_start(sg_enter_idle_timer, sg_response_timeout_ms, TAL_TIMER_ONCE);

    // ASR may complete before VAD STOP. Close that input session here so the
    // next LISTEN cycle cannot remain blocked in AI_INPUT_PROC.
    // The goodbye text input is started after the original audio input has
    // already been stopped. Do not cancel that new text request here.
    if (!sg_goodbye_pending) {
        __ai_mode_free_stop_active_input();
    }
    ai_audio_input_wakeup_set(false);
    if (sg_exit_pending || sg_goodbye_pending) {
        sg_is_wakeup = false;
    } else {
        sg_is_wakeup = true;
    }

    tkl_vad_set_threshold(TKL_AUDIO_VAD_MID);
}

static void __ai_mode_enter_speak(void)
{
#if defined(ENABLE_LED) && (ENABLE_LED == 1)   
    tdl_led_set_status(sg_led_hdl, TDL_LED_ON);
#endif    

    tal_sw_timer_stop(sg_enter_idle_timer);
    if (sg_listen_arm_timer) {
        tal_sw_timer_stop(sg_listen_arm_timer);
    }
    sg_pending_vad_start = false;
    __ai_mode_free_stop_active_input();
    ai_audio_input_wakeup_set(false);
}

static void __ai_mode_listen_arm_time_cb(TIMER_ID timer_id, void *arg)
{
    if (sg_mode_set_state != AI_MODE_STATE_LISTEN || sg_exit_pending || !sg_is_wakeup) {
        return;
    }

    if (ai_audio_player_is_playing()) {
        tal_sw_timer_start(timer_id, AI_CHAT_LISTEN_ARM_RETRY_MS, TAL_TIMER_ONCE);
        return;
    }

    AI_INPUT_STATE_E input_state = tuya_ai_input_get_state();
    if (input_state == AI_INPUT_PROC) {
        PR_WARN("mode free recover stale ai input before listen arm");
        __ai_mode_free_stop_active_input();
        tal_sw_timer_start(timer_id, AI_CHAT_LISTEN_ARM_RETRY_MS, TAL_TIMER_ONCE);
        return;
    }

    if (input_state != AI_INPUT_IDLE) {
        sg_listen_arm_retries++;
        if (sg_listen_arm_retries == 1 ||
            (sg_listen_arm_retries % AI_CHAT_LISTEN_ARM_LOG_RETRIES) == 0) {
            PR_DEBUG("[====ai_free] delay listen arm, ai input state:%d, retry:%d",
                     input_state, sg_listen_arm_retries);
        }
        tal_sw_timer_start(timer_id, AI_CHAT_LISTEN_ARM_RETRY_MS, TAL_TIMER_ONCE);
        return;
    }

    ai_audio_input_output_set(false);
    ai_audio_input_wakeup_set(false);
    ai_audio_input_reset();
    ai_audio_input_wakeup_set(true);
    sg_listen_armed_ms = tal_system_get_millisecond();
    sg_listen_arm_retries = 0;
    PR_DEBUG("mode free listen armed after audio reset");
}

static void __ai_mode_enter_idle_time_cb(TIMER_ID timer_id, void *arg)
{
    if (sg_goodbye_pending) {
        PR_WARN("mode free exit announcement timed out");
        __ai_mode_free_complete_exit();
        return;
    }

    if (ai_audio_player_is_playing()) {
        //! if player is playing, start idle timer again
        PR_NOTICE("player is playing, idle timer reset");
        uint32_t timeout_ms = (sg_mode_set_state == AI_MODE_STATE_LISTEN) ? sg_listen_timeout_ms
                                                                          : sg_response_timeout_ms;
        tal_sw_timer_start(timer_id, timeout_ms, TAL_TIMER_ONCE);
        return;
    }

    __ai_mode_free_begin_exit();
}

static OPERATE_RET __ai_mode_free_init(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(ENABLE_LED) && (ENABLE_LED == 1)
    sg_led_hdl = tdl_led_find_dev(LED_NAME);
    TUYA_CALL_ERR_RETURN(tdl_led_open(sg_led_hdl));
#endif

    //set vad mode
    ai_audio_input_wakeup_mode_set(AI_AUDIO_VAD_AUTO);

    tkl_kws_reg_wakeup_cb(__ai_mode_kws_wakeup);
    tkl_kws_enable();

    //create idle timer
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(__ai_mode_enter_idle_time_cb, NULL, &sg_enter_idle_timer));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(__ai_mode_listen_arm_time_cb, NULL, &sg_listen_arm_timer));

    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
    sg_is_wakeup = false;

    return rt;
}

static OPERATE_RET __ai_mode_free_deinit(void)
{
    tkl_kws_disable();

    ai_audio_input_output_set(false);
    if (sg_enter_idle_timer) {
        tal_sw_timer_stop(sg_enter_idle_timer);
    }
    if (sg_listen_arm_timer) {
        tal_sw_timer_stop(sg_listen_arm_timer);
    }
    tuya_ai_input_stop();

    sg_mode_cur_state = AI_MODE_STATE_INVALID;

    return OPRT_OK;
}

static OPERATE_RET __ai_mode_free_task(void *args)
{
    if(sg_mode_cur_state == sg_mode_set_state) {
        return OPRT_OK;
    }

    switch(sg_mode_set_state) {
        case AI_MODE_STATE_IDLE: {
            __ai_mode_enter_idle();
        }
        break;
        case AI_MODE_STATE_LISTEN: {
            __ai_mode_enter_listen();
        }
        break;
        case AI_MODE_STATE_UPLOAD: {
            __ai_mode_enter_upload();
        }
        break;
        case AI_MODE_STATE_THINK: {
            __ai_mode_enter_think();
        }
        break;
        case AI_MODE_STATE_SPEAK: {
            __ai_mode_enter_speak();
        }
        break;
        default:
        break;
    }

    sg_mode_cur_state = sg_mode_set_state;

    ai_user_event_notify(AI_USER_EVT_MODE_STATE_UPDATE, (void *)sg_mode_cur_state);  

    return OPRT_OK;
}

static OPERATE_RET __ai_mode_free_handle_event(AI_NOTIFY_EVENT_T *event)
{
    TUYA_CHECK_NULL_RETURN(event, OPRT_INVALID_PARM);

    if(event->type != AI_USER_EVT_MIC_DATA && event->type != AI_USER_EVT_TTS_DATA) {
         PR_DEBUG("[====ai_free] event type: %d", event->type);
    }

    switch (event->type) {
        case AI_USER_EVT_ASR_EMPTY:
        case AI_USER_EVT_ASR_ERROR: {
            if (sg_goodbye_pending) {
                PR_WARN("mode free exit announcement returned empty ASR");
                __ai_mode_free_complete_exit();
                break;
            }

            sg_empty_asr_count++;
            if (sg_empty_asr_count >= AI_CHAT_EMPTY_IDLE_LIMIT) {
                PR_NOTICE("mode free exit after %d empty asr", sg_empty_asr_count);
                __ai_mode_free_begin_exit();
            } else {
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
                tal_sw_timer_start(sg_enter_idle_timer, sg_listen_timeout_ms, TAL_TIMER_ONCE);
                __ai_mode_free_schedule_listen_arm(AI_CHAT_LISTEN_ARM_DELAY_MS);
            }
        }
        break;
        case AI_USER_EVT_ASR_OK: {
            __ai_mode_free_update_language((const AI_NOTIFY_TEXT_T *)event->data);
            sg_empty_asr_count = 0;
            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_THINK);
        }
        break;
        case AI_USER_EVT_TTS_PRE: {
            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_SPEAK);
        }
        break;
        case AI_USER_EVT_TEXT_STREAM_START:
        case AI_USER_EVT_TEXT_STREAM_DATA: {
            __ai_mode_free_suppress_duplicate_goodbye((const AI_NOTIFY_TEXT_T *)event->data);
        }
        break;
        case AI_USER_EVT_EXIT: {
            __ai_mode_free_begin_exit();
        }
        break;
        case AI_USER_EVT_TTS_ABORT:
        case AI_USER_EVT_TTS_ERROR: {
            if (sg_goodbye_pending) {
                __ai_mode_free_complete_exit();
            }
        }
        break;
    case AI_USER_EVT_PLAY_CTL_END:
    case AI_USER_EVT_PLAY_END:{
            if (sg_goodbye_pending) {
                __ai_mode_free_complete_exit();
            } else if (sg_is_wakeup && !sg_exit_pending) {
                sg_empty_asr_count = 0;
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
                /* Arm almost immediately after playback ends. Waiting the normal
                 * recovery delay here can reset AFE after the user has already
                 * started a short utterance and clip its first syllable. */
                __ai_mode_free_schedule_listen_arm(AI_CHAT_POST_PLAY_ARM_DELAY_MS);
            } else {
                __ai_mode_free_begin_exit();
            }
        }
        break;        
        default:
        break;
    }

    return OPRT_OK;
}

static AI_MODE_STATE_E __ai_mode_free_get_state(void)
{
    return sg_mode_set_state;
}


static OPERATE_RET __ai_mode_free_client_run(void *data)
{
    PR_NOTICE("connected to server");

    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);

    return OPRT_OK;
}

static OPERATE_RET __ai_mode_free_vad_change(AI_AUDIO_VAD_STATE_E vad_flag)
{
    if(false == sg_is_wakeup) {
        return OPRT_OK;
    }

    if (sg_exit_pending) {
        sg_pending_vad_start = false;
        ai_audio_input_output_set(false);
        return OPRT_OK;
    }

    PR_DEBUG("[====ai_free] vad: [%d]", vad_flag); 

    if (AI_AUDIO_VAD_START == vad_flag) {
        uint32_t now_ms = tal_system_get_millisecond();
        tal_sw_timer_stop(sg_enter_idle_timer);
        PR_DEBUG("mode free vad start %u ms after listen arm",
                 sg_listen_armed_ms ? (unsigned int)(now_ms - sg_listen_armed_ms) : 0U);
        __ai_mode_free_start_input();
    } else {
        sg_pending_vad_start = false;
        ai_audio_input_output_set(false);

        AI_INPUT_STATE_E input_state = tuya_ai_input_get_state();
        if (input_state != AI_INPUT_PROC) {
            PR_DEBUG("[====ai_free] ignore vad stop, ai input state:%d", input_state);
            return OPRT_OK;
        }

        tuya_ai_input_stop();
    }

    return OPRT_OK;
}

#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
static OPERATE_RET __ai_mode_free_handle_key(TDL_BUTTON_TOUCH_EVENT_E event, void *arg)
{
    OPERATE_RET rt = OPRT_OK;

    switch(event) {
        case TDL_BUTTON_PRESS_SINGLE_CLICK: {
            ai_audio_player_stop(AI_AUDIO_PLAYER_ALL);
            ai_audio_input_reset();
            tuya_ai_agent_event(AI_EVENT_CHAT_BREAK, 0);

            ai_audio_player_alert(AI_AUDIO_ALERT_WAKEUP);

            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
            sg_is_wakeup = true;
            sg_exit_pending = false;
            sg_goodbye_pending = false;
            sg_empty_asr_count = 0;
            __ai_mode_free_schedule_listen_arm(AI_CHAT_LISTEN_ARM_DELAY_MS);
        }
        break;
        default:
            break;
    }

    return rt;

}
#endif

OPERATE_RET ai_mode_free_register(void)
{
    OPERATE_RET rt = OPRT_OK;
    AI_MODE_HANDLE_T handle;

    memset(&handle, 0, sizeof(AI_MODE_HANDLE_T));

    handle.name         = FREE_TALK;
    handle.init         = __ai_mode_free_init;
    handle.deinit       = __ai_mode_free_deinit;
    handle.task         = __ai_mode_free_task;
    handle.handle_event = __ai_mode_free_handle_event;
    handle.get_state    = __ai_mode_free_get_state;
    handle.client_run   = __ai_mode_free_client_run;
    handle.vad_change   = __ai_mode_free_vad_change;

#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
    handle.handle_key   = __ai_mode_free_handle_key;
#endif   

    TUYA_CALL_ERR_RETURN(ai_mode_register(AI_CHAT_MODE_FREE, &handle));

    return rt;
}

void ai_mode_free_request_exit(void)
{
    if (sg_mode_set_state == AI_MODE_STATE_IDLE) {
        return;
    }

    PR_NOTICE("mode free request exit after current response");
    sg_exit_pending = true;
    sg_pending_vad_start = false;
    ai_audio_input_output_set(false);
    ai_audio_input_wakeup_set(false);
}
#else 

OPERATE_RET ai_mode_free_register(void)
{
    return OPRT_NOT_SUPPORTED;
}

void ai_mode_free_request_exit(void)
{
}

#endif
