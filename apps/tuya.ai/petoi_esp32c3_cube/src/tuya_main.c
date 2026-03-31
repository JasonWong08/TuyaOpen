/**
 * @file tuya_main.c
 * @brief Main application entry for petoi_esp32c3_cube (ESP32-C3 AI chatbot).
 *
 * Memory optimizations for ESP32-C3 (~400 KB SRAM, no PSRAM):
 *
 *  1. Log buffer reduced to 512 bytes (vs 1024 on PSRAM boards).
 *  2. Application thread stack tuned for C3 runtime stability.
 *  3. No PSRAM allocation path: cJSON always uses internal heap.
 *  4. DMA descriptor count for audio kept small (board_config.h: DMA_DESC_NUM=3).
 *  5. LVGL render buffer is 10 lines × 240 px × 2 B = 4.8 KB DMA SRAM.
 *  6. On-demand / phase-based initialization strategy:
 *       Phase-1  (lightweight): KV, timer, workqueue, CLI, authorize
 *       Phase-2  (network):     IoT init, netmgr — heap logged before/after
 *       Phase-3  (hardware):    board_register_hardware() — audio codec I2C/I2S
 *       Phase-4  (application): app_chat_bot_init() — display + AI pipeline
 *     Between phases, PR_INFO prints free heap so peak consumption is visible.
 *     Audio encode/decode ring-buffers are created inside the AI pipeline only
 *     when an active session starts, and freed on session end (handled by
 *     ai_components — see app_chat_bot.h for the session lifecycle hooks).
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tuya_cloud_types.h"

#include <assert.h>
#include "cJSON.h"
#include "tal_api.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "netmgr.h"
#include "netcfg.h"
#include "tkl_output.h"
#include "tal_cli.h"
#include "tuya_authorize.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#else
#include "tkl_wifi_stub.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "netconn_wired.h"
#endif
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

#include "board_com_api.h"

#include "app_chat_bot.h"
#include "reset_netcfg.h"

#if defined(ENABLE_BATTERY) && (ENABLE_BATTERY == 1)
#include "app_battery.h"
#endif

#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
#include "qrencode_print.h"
#endif
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
#include "ble_mgr.h"
#endif

/* Tuya device handle */
tuya_iot_client_t ai_client;

/* Tuya license information (uuid / authkey) */
tuya_iot_license_t license;

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#define DPID_VOLUME 3

/* Periodic free-heap log interval (ms) */
#define PRINTF_FREE_HEAP_TIME (10 * 1000)

/* Warn when free internal heap falls below this threshold (bytes).
 * With ~400 KB total and audio/display/TCP stacks in flight, 60 KB is a
 * reasonable low-water mark for ESP32-C3. */
#define HEAP_WARN_THRESHOLD         (60 * 1024)
#define HEAP_FREE_CRIT_THRESHOLD    (8 * 1024)
#define HEAP_LARGEST_CRIT_THRESHOLD (4 * 1024)
/* Keep activation/TLS first: offline UI/audio recovery must not run while
 * heap is in the 40~70KB range, otherwise mbedtls_ssl_setup may fail. */
#define OFFLINE_UI_RECOVER_HEAP_MIN    (96 * 1024)
#define OFFLINE_AUDIO_RECOVER_HEAP_MIN (72 * 1024)
#define POSTCLOUD_RETRY_INTERVAL_MS    (2000)

static uint8_t  _need_reset = 0;
static TIMER_ID sg_printf_heap_tm;
static bool     sg_cloud_ready               = false;
static bool     sg_using_fallback_license    = false;
static bool     sg_ble_released              = false;
static bool     sg_ble_release_scheduled     = false;
static bool     sg_postcloud_retry_scheduled = false;
static TIMER_ID sg_postcloud_retry_tm        = NULL;
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
static TIMER_ID sg_ble_release_stage1_tm = NULL;
static TIMER_ID sg_ble_release_stage2_tm = NULL;
#endif

#ifdef PLATFORM_ESP32
extern size_t heap_caps_get_free_size(uint32_t caps);
extern size_t heap_caps_get_minimum_free_size(uint32_t caps);
extern size_t heap_caps_get_largest_free_block(uint32_t caps);

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0x00000008U
#endif
#endif /* PLATFORM_ESP32 */

typedef struct {
    uint64_t boot_ms;
    uint64_t bind_start_ms;
    uint64_t mqtt_connected_ms;
    uint32_t bind_start_count;
    uint32_t mqtt_connected_count;
    uint32_t mqtt_disconnected_count;
} PETOI_RUN_STATS_T;

static PETOI_RUN_STATS_T sg_run_stats = {0};

#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
/* Stage BLE teardown around bind token:
 * 1) stop netcfg/disconnect first, 2) deinit BLE later.
 * This keeps TLS handshake window cleaner on C3 low-heap path. */
#define BLE_RELEASE_STAGE1_DELAY_MS (150)
#define BLE_RELEASE_STAGE2_DELAY_MS (600)

static void __ble_release_stage2_cb(TIMER_ID timer_id, void *arg);

static void __ble_release_timer_cleanup(void)
{
    if (sg_ble_release_stage1_tm) {
        tal_sw_timer_stop(sg_ble_release_stage1_tm);
        tal_sw_timer_delete(sg_ble_release_stage1_tm);
        sg_ble_release_stage1_tm = NULL;
    }

    if (sg_ble_release_stage2_tm) {
        tal_sw_timer_stop(sg_ble_release_stage2_tm);
        tal_sw_timer_delete(sg_ble_release_stage2_tm);
        sg_ble_release_stage2_tm = NULL;
    }
}

static void __ble_release_force_now(const char *reason)
{
    OPERATE_RET stop_rt   = OPRT_OK;
    OPERATE_RET deinit_rt = OPRT_OK;

    if (sg_ble_released) {
        return;
    }

    PR_NOTICE("force BLE release: %s", reason ? reason : "unknown");
    stop_rt = netcfg_stop(NETCFG_TUYA_BLE);
    if (stop_rt != OPRT_OK) {
        PR_WARN("BLE force release: netcfg_stop failed: %d", stop_rt);
    }
    tuya_ble_disconnect_current();
    deinit_rt = tuya_ble_deinit();
    if (deinit_rt != OPRT_OK) {
        /* Do not mark released on failure; caller may schedule retry. */
        sg_ble_released          = false;
        sg_ble_release_scheduled = false;
        __ble_release_timer_cleanup();
        PR_WARN("BLE force release: deinit failed: %d", deinit_rt);
        return;
    }

    sg_ble_released          = true;
    sg_ble_release_scheduled = false;
    __ble_release_timer_cleanup();
    PR_NOTICE("BLE force release done, heap=%d", tal_system_get_free_heap_size());
}

static void __ble_release_stage1_cb(TIMER_ID timer_id, void *arg)
{
    OPERATE_RET rt = OPRT_OK;

    (void)timer_id;
    (void)arg;

    if (sg_ble_released) {
        sg_ble_release_scheduled = false;
        __ble_release_timer_cleanup();
        return;
    }

    PR_NOTICE("BLE release stage1: stop netcfg + disconnect peer");
    TUYA_CALL_ERR_LOG(netcfg_stop(NETCFG_TUYA_BLE));
    tuya_ble_disconnect_current();

    if (sg_ble_release_stage2_tm == NULL) {
        if (tal_sw_timer_create(__ble_release_stage2_cb, NULL, &sg_ble_release_stage2_tm) != OPRT_OK) {
            __ble_release_force_now("stage2 timer create failed");
            return;
        }
    }

    if (tal_sw_timer_start(sg_ble_release_stage2_tm, BLE_RELEASE_STAGE2_DELAY_MS, TAL_TIMER_ONCE) != OPRT_OK) {
        __ble_release_force_now("stage2 timer start failed");
        return;
    }
}

static void __ble_release_stage2_cb(TIMER_ID timer_id, void *arg)
{
    OPERATE_RET deinit_rt = OPRT_OK;

    (void)timer_id;
    (void)arg;

    if (sg_ble_released) {
        sg_ble_release_scheduled = false;
        __ble_release_timer_cleanup();
        return;
    }

    PR_NOTICE("BLE release stage2: deinit BLE stack");
    deinit_rt = tuya_ble_deinit();
    if (deinit_rt != OPRT_OK) {
        sg_ble_release_scheduled = false;
        __ble_release_timer_cleanup();
        PR_WARN("BLE release stage2 failed: %d", deinit_rt);
        return;
    }
    sg_ble_released          = true;
    sg_ble_release_scheduled = false;
    __ble_release_timer_cleanup();
}

static void __schedule_ble_release_after_bind(void)
{
    if (sg_ble_released || sg_ble_release_scheduled) {
        return;
    }

    if (sg_ble_release_stage1_tm == NULL) {
        if (tal_sw_timer_create(__ble_release_stage1_cb, NULL, &sg_ble_release_stage1_tm) != OPRT_OK) {
            __ble_release_force_now("stage1 timer create failed");
            return;
        }
    }

    if (tal_sw_timer_start(sg_ble_release_stage1_tm, BLE_RELEASE_STAGE1_DELAY_MS, TAL_TIMER_ONCE) != OPRT_OK) {
        __ble_release_force_now("stage1 timer start failed");
        return;
    }

    sg_ble_release_scheduled = true;
    PR_NOTICE("BLE release scheduled: stage1=%dms stage2=%dms", BLE_RELEASE_STAGE1_DELAY_MS,
              BLE_RELEASE_STAGE2_DELAY_MS);
}
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
    PR_NOTICE("[heap-snap] %s free=%d", stage ? stage : "unknown", tal_system_get_free_heap_size());
#endif
}

static bool __is_mqtt_connected_now(void)
{
    return (ai_client.status == TUYA_STATUS_MQTT_CONNECTED);
}

static void __postcloud_retry_tm_cb(TIMER_ID timer_id, void *arg);

static void __schedule_postcloud_retry(void)
{
    if (sg_postcloud_retry_scheduled) {
        return;
    }

    if (sg_postcloud_retry_tm == NULL) {
        if (tal_sw_timer_create(__postcloud_retry_tm_cb, NULL, &sg_postcloud_retry_tm) != OPRT_OK) {
            PR_WARN("post-cloud retry: timer create failed");
            return;
        }
    }

    if (tal_sw_timer_start(sg_postcloud_retry_tm, POSTCLOUD_RETRY_INTERVAL_MS, TAL_TIMER_ONCE) != OPRT_OK) {
        PR_WARN("post-cloud retry: timer start failed");
        return;
    }

    sg_postcloud_retry_scheduled = true;
    PR_NOTICE("post-cloud retry scheduled in %d ms", POSTCLOUD_RETRY_INTERVAL_MS);
}

static void __postcloud_retry_tm_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    sg_postcloud_retry_scheduled = false;

    if (!__is_mqtt_connected_now()) {
        PR_DEBUG("post-cloud retry skipped: mqtt disconnected");
        return;
    }

    if (app_chat_bot_is_postcloud_inited()) {
        sg_cloud_ready = true;
        return;
    }

    __log_heap_snapshot("postcloud_retry.before_postcloud_init");
    if (app_chat_bot_postcloud_init() != OPRT_OK) {
        PR_WARN("post-cloud retry: init returned error");
    }

    if (app_chat_bot_is_postcloud_inited()) {
        sg_cloud_ready = true;
        PR_NOTICE("Post-cloud app init recovered, heap=%d", tal_system_get_free_heap_size());
        __log_heap_snapshot("postcloud_retry.after_postcloud_init");
        /* Re-publish after ai_chat_init subscribes EVENT_MQTT_CONNECTED. */
        tal_event_publish(EVENT_MQTT_CONNECTED, NULL);
        return;
    }

    PR_NOTICE("post-cloud init still deferred, heap=%d", tal_system_get_free_heap_size());
    __schedule_postcloud_retry();
}

void user_log_output_cb(const char *str)
{
    tal_uart_write(TUYA_UART_NUM_0, (const uint8_t *)str, strlen(str));
}

void user_upgrade_notify_on(tuya_iot_client_t *client, cJSON *upgrade)
{
    PR_INFO("----- Upgrade information -----");
    PR_INFO("OTA Channel: %d", cJSON_GetObjectItem(upgrade, "type")->valueint);
    PR_INFO("Version: %s", cJSON_GetObjectItem(upgrade, "version")->valuestring);
    PR_INFO("Size: %s", cJSON_GetObjectItem(upgrade, "size")->valuestring);
    PR_INFO("MD5: %s", cJSON_GetObjectItem(upgrade, "md5")->valuestring);
    PR_INFO("HMAC: %s", cJSON_GetObjectItem(upgrade, "hmac")->valuestring);
    PR_INFO("URL: %s", cJSON_GetObjectItem(upgrade, "url")->valuestring);
    PR_INFO("HTTPS URL: %s", cJSON_GetObjectItem(upgrade, "httpsUrl")->valuestring);
}

OPERATE_RET audio_dp_obj_proc(dp_obj_recv_t *dpobj)
{
    uint32_t index = 0;
    for (index = 0; index < dpobj->dpscnt; index++) {
        dp_obj_t *dp = dpobj->dps + index;
        PR_DEBUG("idx:%d dpid:%d type:%d ts:%u", index, dp->id, dp->type, dp->time_stamp);

        switch (dp->id) {
        case DPID_VOLUME: {
            uint8_t volume = dp->value.dp_value;
            PR_DEBUG("volume:%d", volume);
            ai_chat_set_volume(volume);
#if defined(ENABLE_CHAT_DISPLAY) && (ENABLE_CHAT_DISPLAY == 1)
            char volume_str[20] = {0};
            snprintf(volume_str, sizeof(volume_str), "%s%d", VOLUME, volume);
            ai_ui_disp_msg(AI_UI_DISP_NOTIFICATION, (uint8_t *)volume_str, strlen(volume_str));
#endif
            break;
        }
        default:
            break;
        }
    }

    return OPRT_OK;
}

OPERATE_RET ai_audio_volume_upload(void)
{
    tuya_iot_client_t *client = tuya_iot_client_get();
    dp_obj_t           dp_obj = {0};

    uint8_t volume = ai_chat_get_volume();

    dp_obj.id             = DPID_VOLUME;
    dp_obj.type           = PROP_VALUE;
    dp_obj.value.dp_value = volume;

    PR_DEBUG("DP upload volume:%d", volume);

    return tuya_iot_dp_obj_report(client, client->activate.devid, &dp_obj, 1, 0);
}

void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));
    PR_INFO("Device Free heap %d", tal_system_get_free_heap_size());

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        sg_run_stats.bind_start_count++;
        sg_run_stats.bind_start_ms = tal_system_get_millisecond();
        PR_INFO("Device Bind Start!");
        if (_need_reset == 1) {
            /* On C3 memory-tuned path, cloud-triggered reset can race with
             * reconnect and cause reboot loops. Keep running and re-enter
             * bind flow instead of hard reset. */
            PR_WARN("pending reset request detected, skip hard reset on bind start");
            _need_reset = 0;
        }

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
        /* Play bind-start prompt via lightweight offline path so first boot can
         * still have voice feedback even when full post-cloud chain is not ready.
         * The offline player is released at BIND_TOKEN_ON before TLS heavy path. */
        {
            int bind_heap = tal_system_get_free_heap_size();
            if (app_chat_bot_is_ready()) {
                ai_audio_player_alert(AI_AUDIO_ALERT_NETWORK_CFG);
            } else {
                OPERATE_RET ar = app_chat_bot_try_recover_audio_alert(20 * 1024, AI_AUDIO_ALERT_NETWORK_CFG);
                if (ar != OPRT_OK) {
                    PR_WARN("skip bind alert, heap=%d ready=%d rt=%d", bind_heap, app_chat_bot_is_ready(), ar);
                }
            }
        }
#endif

        break;

    case TUYA_EVENT_DIRECT_MQTT_CONNECTED: {
#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
        char buffer[255];
        sprintf(buffer, "https://smartapp.tuya.com/s/p?p=%s&uuid=%s&v=2.0", TUYA_PRODUCT_ID, license.uuid);
        qrcode_string_output(buffer, user_log_output_cb, 0);
#endif
    } break;

    case TUYA_EVENT_BIND_TOKEN_ON:
        /* Release offline-only audio player before WiFi->TLS->MQTT heavy path. */
        {
            OPERATE_RET release_rt = app_chat_bot_release_offline_audio();
            if (release_rt != OPRT_OK) {
                PR_WARN("release offline audio failed: %d", release_rt);
            }
        }
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
        /* Token arrived: immediately quiet BLE to keep TLS handshake window clean.
         * If immediate deinit fails, fallback to staged release retry. */
        __ble_release_force_now("bind_token_immediate");
        if (false == sg_ble_released) {
            __schedule_ble_release_after_bind();
        }
#endif
        PR_NOTICE("Bind token received. waiting WiFi->TLS->MQTT");
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        PR_INFO("Device MQTT Connected!");
        __log_heap_snapshot("mqtt_connected.entry");
        sg_run_stats.mqtt_connected_count++;
        sg_run_stats.mqtt_connected_ms = tal_system_get_millisecond();
        if (sg_run_stats.bind_start_ms > 0 && sg_run_stats.mqtt_connected_ms >= sg_run_stats.bind_start_ms) {
            PR_NOTICE("Bind-to-MQTT latency: %u ms",
                      (unsigned)(sg_run_stats.mqtt_connected_ms - sg_run_stats.bind_start_ms));
        }

        /* After cloud is online, BLE provisioning stack is no longer needed.
         * Releasing BLE can reclaim tens of KB on ESP32-C3 and may allow
         * post-cloud UI/audio init to pass memory threshold. */
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
        if (false == sg_ble_released) {
            __ble_release_force_now("mqtt_connected_fallback");
            tal_system_sleep(100);
            PR_NOTICE("BLE released after MQTT connect, heap=%d", tal_system_get_free_heap_size());
            __log_heap_snapshot("mqtt_connected.after_ble_release");
        }
#endif

        /* Cloud-first strategy:
         * 1) do lightweight UI before bind
         * 2) bring up full AI/audio only after MQTT is stable. */
        if (false == sg_cloud_ready) {
            __log_heap_snapshot("mqtt_connected.before_postcloud_init");
            OPERATE_RET app_rt = app_chat_bot_postcloud_init();
            if (app_rt != OPRT_OK) {
                PR_ERR("post-cloud app init failed: %d", app_rt);
                __log_heap_snapshot("mqtt_connected.postcloud_init_failed");
                break;
            }
            if (app_chat_bot_is_postcloud_inited()) {
                sg_cloud_ready = true;
                PR_NOTICE("Post-cloud app init done, heap=%d", tal_system_get_free_heap_size());
                __log_heap_snapshot("mqtt_connected.after_postcloud_init");
            } else {
                sg_cloud_ready = false;
                PR_WARN("Post-cloud app init deferred, schedule retry");
                __schedule_postcloud_retry();
            }
        }

        tal_event_publish(EVENT_MQTT_CONNECTED, NULL);

        static uint8_t first = 1;
        if (first) {
            first = 0;

#if defined(ENABLE_CHAT_DISPLAY) && (ENABLE_CHAT_DISPLAY == 1)
            UI_WIFI_STATUS_E wifi_status = UI_WIFI_STATUS_GOOD;
            ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&wifi_status, sizeof(UI_WIFI_STATUS_E));
#endif
            if (app_chat_bot_is_ready()) {
                ai_audio_volume_upload();
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
            } else {
                /* In degraded path, AI chat mode may be deferred by heap fragmentation.
                 * Play "network connected" prompt via lightweight offline player so
                 * users still get bind-success voice feedback. */
                OPERATE_RET alert_rt =
                    app_chat_bot_try_recover_audio_alert(24 * 1024, AI_AUDIO_ALERT_NETWORK_CONNECTED);
                if (alert_rt != OPRT_OK) {
                    PR_WARN("skip mqtt-connected alert in degraded mode, rt=%d heap=%d", alert_rt,
                            tal_system_get_free_heap_size());
                }
#endif
            }
        }
        break;

    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_INFO("Device MQTT DisConnected!");
        sg_run_stats.mqtt_disconnected_count++;
        sg_cloud_ready = app_chat_bot_is_postcloud_inited();
        if (sg_postcloud_retry_tm) {
            tal_sw_timer_stop(sg_postcloud_retry_tm);
        }
        sg_postcloud_retry_scheduled = false;
        if (false == sg_cloud_ready) {
            const char *offline_status = CONNECT_SERVER;
            if (sg_using_fallback_license && sg_run_stats.mqtt_connected_count == 0 &&
                sg_run_stats.bind_start_count > 0) {
                offline_status = "Cloud auth failed";
                PR_ERR("MQTT auth rejected before first connect. Check UUID/AuthKey and product binding.");
            }
            if (OPRT_OK != app_chat_bot_try_recover_ui(OFFLINE_UI_RECOVER_HEAP_MIN, offline_status)) {
                PR_DEBUG("disconnect-stage UI recovery skipped");
            }
            if (OPRT_OK !=
                app_chat_bot_try_recover_audio_alert(OFFLINE_AUDIO_RECOVER_HEAP_MIN, AI_AUDIO_ALERT_NETWORK_FAIL)) {
                PR_DEBUG("disconnect-stage audio recovery skipped");
            }
        }
        tal_event_publish(EVENT_MQTT_DISCONNECTED, NULL);
        break;

    case TUYA_EVENT_UPGRADE_NOTIFY:
        user_upgrade_notify_on(client, event->value.asJSON);
        break;

    case TUYA_EVENT_TIMESTAMP_SYNC:
        PR_INFO("Sync timestamp:%d", event->value.asInteger);
        tal_time_set_posix(event->value.asInteger, 1);
        tal_event_publish("app.time.sync", NULL);
        break;

    case TUYA_EVENT_RESET:
        PR_INFO("Device Reset:%d", event->value.asInteger);
        if (sg_using_fallback_license && sg_run_stats.mqtt_connected_count == 0) {
            PR_ERR("cloud reset after MQTT auth reject. likely UUID/AuthKey not accepted by cloud");
            if (OPRT_OK != app_chat_bot_try_recover_ui(OFFLINE_UI_RECOVER_HEAP_MIN, "Cloud auth failed")) {
                PR_DEBUG("reset-stage UI recovery skipped");
            }
            if (OPRT_OK !=
                app_chat_bot_try_recover_audio_alert(OFFLINE_AUDIO_RECOVER_HEAP_MIN, AI_AUDIO_ALERT_NOT_ACTIVE)) {
                PR_DEBUG("reset-stage audio recovery skipped");
            }
        }
        /* Keep a marker for observability, but do not reboot here. */
        _need_reset = 1;
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        dp_obj_recv_t *dpobj = event->value.dpobj;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d CNT:%u", dpobj->cmd_tp, dpobj->dtt_tp, dpobj->dpscnt);
        if (dpobj->devid != NULL) {
            PR_DEBUG("devid.%s", dpobj->devid);
        }

        audio_dp_obj_proc(dpobj);
        tuya_iot_dp_obj_report(client, dpobj->devid, dpobj->dps, dpobj->dpscnt, 0);
    } break;

    case TUYA_EVENT_DP_RECEIVE_RAW: {
        dp_raw_recv_t *dpraw = event->value.dpraw;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d", dpraw->cmd_tp, dpraw->dtt_tp);
        if (dpraw->devid != NULL) {
            PR_DEBUG("devid.%s", dpraw->devid);
        }

        uint32_t  index = 0;
        dp_raw_t *dp    = &dpraw->dp;
        PR_DEBUG("dpid:%d type:RAW len:%d data:", dp->id, dp->len);
        for (index = 0; index < dp->len; index++) {
            PR_DEBUG_RAW("%02x", dp->data[index]);
        }

        tuya_iot_dp_raw_report(client, dpraw->devid, &dpraw->dp, 3);
    } break;

    default:
        break;
    }
}

bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status == NETMGR_LINK_DOWN ? false : true;
}

static void __printf_heap_tm_cb(TIMER_ID timer_id, void *arg)
{
#ifdef PLATFORM_ESP32
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    PR_INFO("Heap: free=%-6u", (unsigned)free_now);

    if (free_now < HEAP_WARN_THRESHOLD) {
        PR_WARN("Low heap! free=%u bytes (threshold=%u). "
                "Consider reducing audio buffer or deferring display init.",
                (unsigned)free_now, (unsigned)HEAP_WARN_THRESHOLD);
    }
    /* Keep periodic monitor lightweight on ESP32-C3: walking TLSF pools for
     * largest-block stats can itself trip when heap metadata is already
     * damaged by earlier OOM/fragmentation pressure. */
    if (free_now < HEAP_FREE_CRIT_THRESHOLD) {
        PR_ERR("Heap critical! free=%u (crit_free=%u). TLS/WiFi may fail due to fragmentation.", (unsigned)free_now,
               (unsigned)HEAP_FREE_CRIT_THRESHOLD);
    }
#else
    PR_INFO("Heap: free=%d", tal_system_get_free_heap_size());
#endif
}

void user_main(void)
{
    int ret = OPRT_OK;
    memset(&sg_run_stats, 0, sizeof(sg_run_stats));
    sg_run_stats.boot_ms = tal_system_get_millisecond();

    /* ── Phase 1: Minimal bootstrap ──────────────────────────────────────
     * ESP32-C3 has no PSRAM. Always use internal heap for cJSON.
     * Log buffer is 512 bytes (half of the default 1024) to save SRAM.
     * -------------------------------------------------------------------*/
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 512, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_sw_timer_create(__printf_heap_tm_cb, NULL, &sg_printf_heap_tm);
    tal_sw_timer_start(sg_printf_heap_tm, PRINTF_FREE_HEAP_TIME, TAL_TIMER_CYCLE);
    tal_workq_init();
    tal_time_service_init();
    tal_cli_init();
    tuya_authorize_init();

    reset_netconfig_start();

    PR_INFO("[Phase-1 done] Heap after bootstrap: %d", tal_system_get_free_heap_size());

    /* ── Phase 2: Cloud & network init ───────────────────────────────────
     * tuya_iot_init allocates the MQTT + TLS context (~30–50 KB).
     * Log heap before and after to track the cost.
     * -------------------------------------------------------------------*/
    if (OPRT_OK != tuya_authorize_read(&license)) {
        license.uuid              = TUYA_OPENSDK_UUID;
        license.authkey           = TUYA_OPENSDK_AUTHKEY;
        sg_using_fallback_license = true;
        PR_WARN("Replace UUID/AuthKey in tuya_config.h or provision via platform.");
    }

    ret = tuya_iot_init(&ai_client, &(const tuya_iot_config_t){
                                        .software_ver  = PROJECT_VERSION,
                                        .productkey    = TUYA_PRODUCT_ID,
                                        .uuid          = license.uuid,
                                        .authkey       = license.authkey,
                                        .event_handler = user_event_handler_on,
                                        .network_check = user_network_check,
                                    });
    assert(ret == OPRT_OK);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
    netmgr_init(type);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    /* ESP32-C3 无 PSRAM：BLE + SoftAP 并存时 WiFi 易分配不到 event beacon 缓冲
     *（日志: alloc eb len=752 fail → ieee80211_hostap_attach 崩溃）。仅使用 BLE 配网即可。 */
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
#endif

    PR_INFO("[Phase-2 done] Heap after cloud/net init: %d", tal_system_get_free_heap_size());

    /* ── Phase 3: Hardware registration ──────────────────────────────────
     * Registers ES8311 audio codec (I2C + I2S buses) and BOOT button.
     * The ST7789 LCD is initialised on-demand inside app_chat_bot_init()
     * via board_display_init(), so its LVGL buffer is not allocated here.
     * -------------------------------------------------------------------*/
    ret = board_register_hardware();
    if (ret != OPRT_OK) {
        PR_ERR("board_register_hardware failed: %d", ret);
    }

    PR_INFO("[Phase-3 done] Heap after hardware init: %d", tal_system_get_free_heap_size());

    /* ── Phase 4: Pre-cloud minimal app init ─────────────────────────────
     * Keep only minimal UI/health display here. Full AI/audio is deferred
     * until MQTT connected to avoid TLS allocation failures on ESP32-C3.
     * -------------------------------------------------------------------*/
    ret = app_chat_bot_precloud_init();
    if (ret != OPRT_OK) {
        PR_ERR("app_chat_bot_precloud_init failed: %d", ret);
    }

#if defined(ENABLE_BATTERY) && (ENABLE_BATTERY == 1)
    ret = app_battery_init();
    if (ret != OPRT_OK) {
        PR_ERR("app_battery_init failed: %d", ret);
    }
#endif

    PR_INFO("[Phase-4 done] Heap after pre-cloud app init: %d", tal_system_get_free_heap_size());

    tuya_iot_start(&ai_client);

    tkl_wifi_set_lp_mode(0, 0);

    reset_netconfig_check();

    for (;;) {
        tuya_iot_yield(&ai_client);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    /* MQTT/TLS + reset callback chain can overflow 3 KB stack on C3.
     * Restore safer stack depth to avoid runtime stack protection faults. */
    thrd_param.stackDepth = 4096;
    thrd_param.priority   = 4;
    thrd_param.thrdname   = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
