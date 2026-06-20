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

#define AI_CHAT_WAKEUP_TIME_MS     (30 * 1000)      // 30sec
#define AI_CHAT_LISTEN_ARM_DELAY_MS 250
#define AI_CHAT_LISTEN_ARM_RETRY_MS 100
#define AI_CHAT_LISTEN_ARM_LOG_RETRIES 10
#define AI_CHAT_EMPTY_IDLE_LIMIT    2

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
static uint8_t         sg_empty_asr_count = 0;
static uint8_t         sg_listen_arm_retries = 0;
static TIMER_ID        sg_enter_idle_timer = NULL;
static TIMER_ID        sg_listen_arm_timer = NULL;
static uint32_t        sg_wakeup_time_ms = AI_CHAT_WAKEUP_TIME_MS;

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
    PR_NOTICE("mode free listen arm scheduled in %d ms", delay_ms);
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
    sg_empty_asr_count = 0;

    tkl_vad_set_threshold(TKL_AUDIO_VAD_LOW);
}

static void __ai_mode_enter_listen(void)
{
#if defined(ENABLE_LED) && (ENABLE_LED == 1)   
    tdl_led_flash(sg_led_hdl, 500);
#endif

    tal_sw_timer_start(sg_enter_idle_timer, sg_wakeup_time_ms, TAL_TIMER_ONCE);

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

    tal_sw_timer_start(sg_enter_idle_timer, sg_wakeup_time_ms, TAL_TIMER_ONCE);

    // ASR may complete before VAD STOP. Close that input session here so the
    // next LISTEN cycle cannot remain blocked in AI_INPUT_PROC.
    __ai_mode_free_stop_active_input();
    ai_audio_input_wakeup_set(false);
    if (sg_exit_pending) {
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
    sg_listen_arm_retries = 0;
    PR_NOTICE("mode free listen armed after audio reset");
}

static void __ai_mode_enter_idle_time_cb(TIMER_ID timer_id, void *arg)
{
    if (ai_audio_player_is_playing()) {
        //! if player is playing, start idle timer again
        PR_NOTICE("player is playing, idle timer reset");
        tal_sw_timer_start(timer_id, sg_wakeup_time_ms, TAL_TIMER_ONCE);
        return;
    } 

    MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
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
            sg_empty_asr_count++;
            if (sg_empty_asr_count >= AI_CHAT_EMPTY_IDLE_LIMIT) {
                PR_NOTICE("mode free idle after %d empty asr", sg_empty_asr_count);
                sg_is_wakeup = false;
                sg_pending_vad_start = false;
                ai_audio_input_output_set(false);
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
            } else {
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
                __ai_mode_free_schedule_listen_arm(AI_CHAT_LISTEN_ARM_DELAY_MS);
            }
        }
        break;
        case AI_USER_EVT_ASR_OK: {
            sg_empty_asr_count = 0;
            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_THINK);
        }
        break;
        case AI_USER_EVT_TTS_PRE: {
            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_SPEAK);
        }
        break;
        case AI_USER_EVT_EXIT: {
            sg_is_wakeup = false;
            sg_pending_vad_start = false;
            sg_exit_pending = false;
            ai_audio_input_output_set(false);
            MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
        }
        break;
    case AI_USER_EVT_PLAY_CTL_END:
    case AI_USER_EVT_PLAY_END:{
            if (sg_is_wakeup && !sg_exit_pending) {
                sg_empty_asr_count = 0;
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_LISTEN);
                __ai_mode_free_schedule_listen_arm(AI_CHAT_LISTEN_ARM_DELAY_MS);
            } else {
                sg_is_wakeup = false;
                sg_pending_vad_start = false;
                sg_exit_pending = false;
                sg_empty_asr_count = 0;
                ai_audio_input_output_set(false);
                MODE_STATE_CHANGE(sg_mode_set_state, AI_MODE_STATE_IDLE);
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
