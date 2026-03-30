/**
 * @file app_chat_bot.c
 * @brief app_chat_bot module is used to
 * @version 0.1
 * @date 2025-03-25
 */

#include "tal_api.h"

#include "netmgr.h"

#include "ai_chat_main.h"
#include "app_chat_bot.h"

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "tkl_wifi.h"
#endif
/***********************************************************
************************macro define************************
***********************************************************/
#define PRINTF_FREE_HEAP_TTIME (10 * 1000)
#define DISP_NET_STATUS_TIME   (1 * 1000)
/* Keep TLS path first on ESP32-C3: if pre-cloud heap is lower than this,
 * delay LVGL/UI startup until cloud is connected. */
#define PRECLOUD_UI_HEAP_MIN (90 * 1024)
/* After MQTT connect, system heap can still dip below 10KB on C3.
 * Starting AI+LVGL in this window easily causes watchdog/reset. */
#define POSTCLOUD_INIT_HEAP_MIN (20 * 1024)
/* Audio-priority mode:
 * prioritize full AI audio chain after MQTT if memory allows. */
#define POSTCLOUD_AUDIO_INIT_HEAP_MIN    (32 * 1024)
#define POSTCLOUD_AUDIO_LARGEST_HEAP_MIN (20 * 1024)

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************const declaration********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/
static TIMER_ID sg_printf_heap_tm;
static bool     sg_precloud_inited       = false;
static bool     sg_postcloud_inited      = false;
static bool     sg_postcloud_in_progress = false;
static bool     sg_ui_inited             = false;
static bool     sg_postcloud_degraded    = false;
static bool     sg_offline_audio_inited  = false;

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static AI_UI_WIFI_STATUS_E sg_wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
static TIMER_ID            sg_disp_status_tm;
extern OPERATE_RET         ai_chat_ui_init(void);
static void                __display_status_tm_cb(TIMER_ID timer_id, void *arg);
#endif

#ifdef PLATFORM_ESP32
extern size_t heap_caps_get_free_size(uint32_t caps);
extern size_t heap_caps_get_minimum_free_size(uint32_t caps);
extern size_t heap_caps_get_largest_free_block(uint32_t caps);

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0x00000008U
#endif
#endif

static void __log_heap_snapshot(const char *stage)
{
#ifdef PLATFORM_ESP32
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_ever = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    PR_NOTICE("[heap-snap] %s free=%u min_ever=%u largest=%u", stage ? stage : "unknown", (unsigned)free_now,
              (unsigned)min_ever, (unsigned)largest);
#else
    PR_NOTICE("[heap-snap] %s free=%u", stage ? stage : "unknown", (unsigned)tal_system_get_free_heap_size());
#endif
}

static void __get_heap_snapshot(uint32_t *free_heap, uint32_t *largest_heap)
{
#ifdef PLATFORM_ESP32
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
    size_t free_now = tal_system_get_free_heap_size();
    size_t largest  = free_now;
#endif

    if (free_heap) {
        *free_heap = (uint32_t)free_now;
    }
    if (largest_heap) {
        *largest_heap = (uint32_t)largest;
    }
}

/* Display timer helper is shared by normal and degraded paths. */
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static OPERATE_RET __ensure_display_status_timer(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (NULL == sg_disp_status_tm) {
        TUYA_CALL_ERR_RETURN(tal_sw_timer_create(__display_status_tm_cb, NULL, &sg_disp_status_tm));
        TUYA_CALL_ERR_RETURN(tal_sw_timer_start(sg_disp_status_tm, DISP_NET_STATUS_TIME, TAL_TIMER_CYCLE));
    }

    return rt;
}

static OPERATE_RET __ensure_ui_ready(const char *status_text)
{
    OPERATE_RET rt = OPRT_OK;

    if (false == sg_ui_inited) {
        TUYA_CALL_ERR_RETURN(ai_chat_ui_init());
        sg_ui_inited = true;
        PR_NOTICE("ui init done, heap=%u", (unsigned)tal_system_get_free_heap_size());
    }

    ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&sg_wifi_status, sizeof(AI_UI_WIFI_STATUS_E));
    if (status_text && status_text[0] != '\0') {
        ai_ui_disp_msg(AI_UI_DISP_STATUS, (uint8_t *)status_text, strlen(status_text));
    }
    ai_ui_disp_msg(AI_UI_DISP_EMOTION, (uint8_t *)EMOJI_NEUTRAL, strlen(EMOJI_NEUTRAL));

    return __ensure_display_status_timer();
}
#endif

/***********************************************************
***********************function define**********************
***********************************************************/
static void __printf_free_heap_tm_cb(TIMER_ID timer_id, void *arg)
{
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    uint32_t free_heap       = tal_system_get_free_heap_size();
    uint32_t free_psram_heap = tal_psram_get_free_heap_size();
    PR_INFO("Free heap size:%d, Free psram heap size:%d", free_heap, free_psram_heap);
#else
    uint32_t free_heap = tal_system_get_free_heap_size();
    PR_INFO("Free heap size:%d", free_heap);
#endif
}

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static void __display_net_status_update(void)
{
    AI_UI_WIFI_STATUS_E wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    netmgr_status_e     net_status  = NETMGR_LINK_DOWN;

    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &net_status);
    if (net_status == NETMGR_LINK_UP) {
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
        // get rssi
        int8_t rssi = 0;
#ifndef PLATFORM_T5
        // BUG: Getting RSSI causes a crash on T5 platform
        tkl_wifi_station_get_conn_ap_rssi(&rssi);
#endif
        if (rssi >= -60) {
            wifi_status = AI_UI_WIFI_STATUS_GOOD;
        } else if (rssi >= -70) {
            wifi_status = AI_UI_WIFI_STATUS_FAIR;
        } else {
            wifi_status = AI_UI_WIFI_STATUS_WEAK;
        }
#else
        wifi_status = AI_UI_WIFI_STATUS_GOOD;
#endif
    } else {
        wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    }

    if (wifi_status != sg_wifi_status) {
        sg_wifi_status = wifi_status;
        ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&wifi_status, sizeof(AI_UI_WIFI_STATUS_E));
    }
}

static void __display_status_tm_cb(TIMER_ID timer_id, void *arg)
{
    __display_net_status_update();
}

#endif

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
static void __ai_video_display_flush(TDL_CAMERA_FRAME_T *frame)
{
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    ai_ui_camera_flush(frame->data, frame->width, frame->height);
#endif
}
#endif

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
static void __ai_picture_output_notify_cb(AI_PICTURE_OUTPUT_NOTIFY_T *info)
{
    OPERATE_RET rt = OPRT_OK;

    if (NULL == info) {
        return;
    }

    if (AI_PICTURE_OUTPUT_START == info->event) {
        AI_PICTURE_CONVERT_CFG_T convert_cfg = {
            .in_fmt        = TUYA_FRAME_FMT_JPEG,
            .in_frame_size = info->total_size,
            .out_fmt       = TUYA_FRAME_FMT_RGB565,
        };

        TUYA_CALL_ERR_LOG(ai_picture_convert_start(&convert_cfg));
    } else if (AI_PICTURE_OUTPUT_SUCCESS == info->event) {
        AI_PICTURE_INFO_T picture_info;

        memset(&picture_info, 0, sizeof(AI_PICTURE_INFO_T));

        TUYA_CALL_ERR_LOG(ai_picture_convert(&picture_info));
        if (rt == OPRT_OK) {
            PR_NOTICE("Picture convert success: fmt=%d, width=%d, height=%d, size=%d", picture_info.fmt,
                      picture_info.width, picture_info.height, picture_info.frame_size);
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
            ai_ui_disp_picture(picture_info.fmt, picture_info.width, picture_info.height, picture_info.frame,
                               picture_info.frame_size);

#endif
        }

        TUYA_CALL_ERR_LOG(ai_picture_convert_stop());
    } else if (AI_PICTURE_OUTPUT_FAILED == info->event) {
        TUYA_CALL_ERR_LOG(ai_picture_convert_stop());
    } else {
        ;
    }
}

void __ai_picture_output_cb(uint8_t *data, uint32_t len, bool is_eof)
{
    ai_picture_convert_feed(data, len);
}

#endif

static void __ai_chat_handle_event(AI_NOTIFY_EVENT_T *event)
{
    (void)event;
}

OPERATE_RET app_chat_bot_precloud_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (sg_precloud_inited) {
        return OPRT_OK;
    }

    // Free heap size
    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(__printf_free_heap_tm_cb, NULL, &sg_printf_heap_tm));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_start(sg_printf_heap_tm, PRINTF_FREE_HEAP_TTIME, TAL_TIMER_CYCLE));

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    /* On C3 no-PSRAM boards, LVGL + display task can consume heap that TLS setup
     * urgently needs. Defer UI when heap is tight. */
    uint32_t precloud_heap = tal_system_get_free_heap_size();
    if (precloud_heap >= PRECLOUD_UI_HEAP_MIN) {
        TUYA_CALL_ERR_RETURN(ai_chat_ui_init());
        sg_ui_inited = true;

        ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&sg_wifi_status, sizeof(AI_UI_WIFI_STATUS_E));
        ai_ui_disp_msg(AI_UI_DISP_STATUS, (uint8_t *)INITIALIZING, strlen(INITIALIZING));
        ai_ui_disp_msg(AI_UI_DISP_EMOTION, (uint8_t *)EMOJI_NEUTRAL, strlen(EMOJI_NEUTRAL));

        // display status update
        TUYA_CALL_ERR_RETURN(tal_sw_timer_create(__display_status_tm_cb, NULL, &sg_disp_status_tm));
        TUYA_CALL_ERR_RETURN(tal_sw_timer_start(sg_disp_status_tm, DISP_NET_STATUS_TIME, TAL_TIMER_CYCLE));
    } else {
        PR_WARN("defer pre-cloud UI, heap=%u < %u", (unsigned)precloud_heap, (unsigned)PRECLOUD_UI_HEAP_MIN);
    }
#endif

    sg_precloud_inited = true;
    return OPRT_OK;
}

OPERATE_RET app_chat_bot_postcloud_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (sg_postcloud_inited) {
        __log_heap_snapshot("postcloud_init.already_inited");
        return OPRT_OK;
    }

    /* Prevent nested/concurrent entry (e.g. MQTT/event churn) from running
     * ai_chat_init + codec_8311_init twice — major TLSF heap corruption risk. */
    if (sg_postcloud_in_progress) {
        PR_WARN("postcloud_init: reentry ignored (init in progress)");
        return OPRT_OK;
    }
    sg_postcloud_in_progress = true;

    uint32_t postcloud_heap = tal_system_get_free_heap_size();
    __log_heap_snapshot("postcloud_init.entry");
    if (postcloud_heap < POSTCLOUD_INIT_HEAP_MIN) {
        sg_postcloud_degraded = true;
        /* Keep cloud online first; postpone AI/UI to avoid WDT. */
        PR_WARN("skip post-cloud AI/UI init, heap=%u < %u", (unsigned)postcloud_heap,
                (unsigned)POSTCLOUD_INIT_HEAP_MIN);
        __log_heap_snapshot("postcloud_init.skip_below_threshold");
        goto postcloud_exit;
    }

    uint32_t free_heap = 0, largest_heap = 0;
    __get_heap_snapshot(&free_heap, &largest_heap);
    if ((free_heap < POSTCLOUD_AUDIO_INIT_HEAP_MIN) || (largest_heap < POSTCLOUD_AUDIO_LARGEST_HEAP_MIN)) {
        sg_postcloud_degraded = true;
        PR_WARN("audio-priority: defer ai init, free=%u largest=%u (need free>=%u largest>=%u)", (unsigned)free_heap,
                (unsigned)largest_heap, (unsigned)POSTCLOUD_AUDIO_INIT_HEAP_MIN,
                (unsigned)POSTCLOUD_AUDIO_LARGEST_HEAP_MIN);
        __log_heap_snapshot("postcloud_init.defer_ai");
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
        /* Degraded path still keeps status visible, but does not block on display. */
        TUYA_CALL_ERR_LOG(__ensure_ui_ready(CONNECT_SERVER));
#endif
        goto postcloud_exit;
    }

    AI_CHAT_MODE_CFG_T ai_chat_cfg = {
        .default_mode = AI_CHAT_MODE_WAKEUP,
        .default_vol  = 70,
        .evt_cb       = __ai_chat_handle_event,
    };
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    if (sg_offline_audio_inited) {
        /* Offline alert path used standalone audio player. Deinit it before
         * ai_chat_init() takes over audio modules. */
        TUYA_CALL_ERR_LOG(ai_audio_player_deinit());
        sg_offline_audio_inited = false;
    }
#endif
    __log_heap_snapshot("postcloud_init.before_ai_chat_init");
    rt = ai_chat_init(&ai_chat_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("ai_chat_init failed: %d", rt);
        __log_heap_snapshot("postcloud_init.ai_chat_init_failed");
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
        TUYA_CALL_ERR_LOG(__ensure_ui_ready(CONNECT_SERVER));
#endif
        sg_postcloud_degraded = true;
        goto postcloud_exit;
    }
    /* ai_chat_init() now owns UI init; mark UI as ready for fallback paths. */
    sg_ui_inited = true;
    __log_heap_snapshot("postcloud_init.after_ai_chat_init");

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
    AI_VIDEO_CFG_T ai_video_cfg = {
        .disp_flush_cb = __ai_video_display_flush,
    };
    TUYA_CALL_ERR_LOG(ai_video_init(&ai_video_cfg));
#endif

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
    rt = ai_mcp_init();
    if (rt != OPRT_OK) {
        PR_ERR("ai_mcp_init failed: %d", rt);
        sg_postcloud_in_progress = false;
        return rt;
    }
#endif

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
    AI_PICTURE_OUTPUT_CFG_T picture_output_cfg = {
        .notify_cb = __ai_picture_output_notify_cb,
        .output_cb = __ai_picture_output_cb,
    };

    rt = ai_picture_output_init(&picture_output_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("ai_picture_output_init failed: %d", rt);
        sg_postcloud_in_progress = false;
        return rt;
    }
#endif

    sg_postcloud_inited = true;
    __log_heap_snapshot("postcloud_init.exit");

postcloud_exit:
    sg_postcloud_in_progress = false;
    return OPRT_OK;
}

OPERATE_RET app_chat_bot_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    TUYA_CALL_ERR_RETURN(app_chat_bot_precloud_init());
    TUYA_CALL_ERR_RETURN(app_chat_bot_postcloud_init());
    return OPRT_OK;
}

bool app_chat_bot_is_ready(void)
{
    return sg_postcloud_inited && (false == sg_postcloud_degraded);
}

OPERATE_RET app_chat_bot_try_recover_ui(uint32_t min_heap_bytes, const char *status_text)
{
    OPERATE_RET rt = OPRT_OK;
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    uint32_t free_heap = tal_system_get_free_heap_size();
    if (free_heap < min_heap_bytes) {
        PR_WARN("defer offline UI recovery, heap=%u < %u", (unsigned)free_heap, (unsigned)min_heap_bytes);
        return OPRT_COM_ERROR;
    }

    TUYA_CALL_ERR_RETURN(__ensure_ui_ready(status_text));
    PR_NOTICE("offline UI recovery init done, heap=%u", (unsigned)tal_system_get_free_heap_size());
    return rt;
#else
    (void)min_heap_bytes;
    (void)status_text;
    return OPRT_NOT_SUPPORTED;
#endif
}

OPERATE_RET app_chat_bot_try_recover_audio_alert(uint32_t min_heap_bytes, AI_AUDIO_ALERT_TYPE_E alert)
{
    OPERATE_RET rt = OPRT_OK;
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    uint32_t free_heap = tal_system_get_free_heap_size();
    if (free_heap < min_heap_bytes) {
        PR_WARN("defer offline audio recovery, heap=%u < %u", (unsigned)free_heap, (unsigned)min_heap_bytes);
        return OPRT_COM_ERROR;
    }

    if (false == sg_offline_audio_inited) {
        TUYA_CALL_ERR_RETURN(ai_audio_player_init());
        TUYA_CALL_ERR_LOG(ai_audio_player_set_vol(70));
        sg_offline_audio_inited = true;
        PR_NOTICE("offline audio recovery init done, heap=%u", (unsigned)tal_system_get_free_heap_size());
    }

    TUYA_CALL_ERR_LOG(ai_audio_player_alert(alert));
    return OPRT_OK;
#else
    (void)min_heap_bytes;
    (void)alert;
    return OPRT_NOT_SUPPORTED;
#endif
}

OPERATE_RET app_chat_bot_release_offline_audio(void)
{
    OPERATE_RET rt = OPRT_OK;
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    if (sg_offline_audio_inited) {
        /* Deinit may race with still-draining player queue right after alert EOF.
         * Retry briefly so SERVICE_DEINIT can be posted and resources are freed. */
        for (int i = 0; i < 3; i++) {
            rt = ai_audio_player_deinit();
            if (rt == OPRT_OK) {
                break;
            }
            tal_system_sleep(30);
        }
        TUYA_CALL_ERR_LOG(rt);
        sg_offline_audio_inited = false;
        PR_NOTICE("offline audio recovery released, heap=%u", (unsigned)tal_system_get_free_heap_size());
    }
#endif
    return OPRT_OK;
}
