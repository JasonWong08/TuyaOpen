/**
 * @file tuya_main.c
 * @brief Implements main audio functionality for IoT device
 *
 * This source file provides the implementation of the main audio functionalities
 * required for an IoT device. It includes functionality for audio processing,
 * device initialization, event handling, and network communication. The
 * implementation supports audio volume control, data point processing, and
 * interaction with the Tuya IoT platform. This file is essential for developers
 * working on IoT applications that require audio capabilities and integration
 * with the Tuya IoT ecosystem.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include "tuya_cloud_types.h"

#include <assert.h>
#include "cJSON.h"
#include "tal_api.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "netmgr.h"
#include "tkl_output.h"
#include "tal_cli.h"
#include "tal_kv.h"
#include "tuya_authorize.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#else
// Stub WiFi functions for non-WiFi platforms (e.g., Ubuntu with wired)
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
#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
#include "ai_mcp.h"
#endif
#include "quaddle_ble_hid_central.h"
#include "reset_netcfg.h"

#if defined(ENABLE_BATTERY) && (ENABLE_BATTERY == 1)
#include "app_battery.h"
#endif

#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
#include "qrencode_print.h"
#endif

/* Tuya device handle */
tuya_iot_client_t ai_client;

/* Tuya license information (uuid authkey) */
tuya_iot_license_t license;

static const cli_cmd_t sg_kv_cli_cmd[] = {
    {
        .name = "kv",
        .help = "kv set/get/del/list",
        .func = tal_kv_cmd,
    },
};

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#define DPID_VOLUME 6

/* Periodic free-heap log interval (ms) */
#define PRINTF_FREE_HEAP_TIME      (10 * 1000)
#define NETWORK_CFG_ALERT_DELAY_MS (1500)

static uint8_t  _need_reset = 0;
static TIMER_ID sg_printf_heap_tm;
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
static TIMER_ID sg_network_cfg_alert_tm;
#endif

/**
 * @brief user defined log output api, in this demo, it will use uart0 as log-tx
 *
 * @param str log string
 * @return void
 */
void user_log_output_cb(const char *str)
{
    tal_uart_write(TUYA_UART_NUM_0, (const uint8_t *)str, strlen(str));
}

/**
 * @brief user defined upgrade notify callback, it will notify device a OTA request received
 *
 * @param client device info
 * @param upgrade the upgrade request info
 * @return void
 */
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

static OPERATE_RET __dp_obj_report(tuya_iot_client_t *client, dp_obj_recv_t *dpobj)
{
    const char *devid = NULL;
    OPERATE_RET rt    = OPRT_OK;

    if ((client == NULL) || (dpobj == NULL)) {
        return OPRT_INVALID_PARM;
    }

    devid = (dpobj->devid != NULL) ? dpobj->devid : client->activate.devid;
    rt    = tuya_iot_dp_obj_report(client, devid, dpobj->dps, dpobj->dpscnt, 0);
    if (rt == OPRT_SVC_DP_ID_NOT_FOUND) {
        PR_DEBUG("skip dp report: no changed dp");
        return OPRT_OK;
    }
    if (rt != OPRT_OK) {
        PR_WARN("dp obj report failed:%d", rt);
    }

    return rt;
}

OPERATE_RET audio_dp_obj_proc(dp_obj_recv_t *dpobj, bool *need_report)
{
    uint32_t index = 0;

    if (need_report != NULL) {
        *need_report = false;
    }

    for (index = 0; index < dpobj->dpscnt; index++) {
        dp_obj_t *dp = dpobj->dps + index;
        PR_DEBUG("idx:%d dpid:%d type:%d ts:%u", index, dp->id, dp->type, dp->time_stamp);

        switch (dp->id) {
        case DPID_VOLUME: {
            uint8_t old_volume = ai_chat_get_volume();
            uint8_t volume = dp->value.dp_value;
            OPERATE_RET rt = OPRT_OK;

            PR_DEBUG("volume:%d", volume);
            rt = ai_chat_set_volume(volume);
            TUYA_CALL_ERR_LOG(rt);
            if ((rt == OPRT_OK) && (need_report != NULL) && (old_volume != volume)) {
                *need_report = true;
            }
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

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
static OPERATE_RET __mcp_volume_changed_cb(int volume, void *user_data)
{
    (void)volume;
    (void)user_data;

    return ai_audio_volume_upload();
}
#endif

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
static void __network_cfg_alert_tm_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;

    PR_NOTICE("Play delayed network configuration alert");
    ai_audio_player_alert(AI_AUDIO_ALERT_NETWORK_CFG);
}

static void __schedule_network_cfg_alert(void)
{
    OPERATE_RET rt = OPRT_OK;

    if (sg_network_cfg_alert_tm == NULL) {
        TUYA_CALL_ERR_LOG(tal_sw_timer_create(__network_cfg_alert_tm_cb, NULL, &sg_network_cfg_alert_tm));
    }

    if (sg_network_cfg_alert_tm) {
        tal_sw_timer_stop(sg_network_cfg_alert_tm);
        TUYA_CALL_ERR_LOG(tal_sw_timer_start(sg_network_cfg_alert_tm, NETWORK_CFG_ALERT_DELAY_MS, TAL_TIMER_ONCE));
    }
}
#endif

/**
 * @brief user defined event handler
 *
 * @param client device info
 * @param event the event info
 * @return void
 */
void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));
    PR_INFO("Device Free heap %d", tal_system_get_free_heap_size());

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("Device Bind Start!");
        if (_need_reset == 1) {
            PR_INFO("Device Reset!");
            tal_system_reset();
        }

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
        __schedule_network_cfg_alert();
#endif

        break;

    /* Print the QRCode for Tuya APP bind */
    case TUYA_EVENT_DIRECT_MQTT_CONNECTED: {
#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
        char buffer[255];
        sprintf(buffer, "https://smartapp.tuya.com/s/p?p=%s&uuid=%s&v=2.0", TUYA_PRODUCT_ID, license.uuid);
        qrcode_string_output(buffer, user_log_output_cb, 0);
#endif
    } break;

    case TUYA_EVENT_BIND_TOKEN_ON:
#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL
        quaddle_ble_hid_central_set_wifi_busy(true);
#endif
        break;

    /* MQTT with tuya cloud is connected, device online */
    case TUYA_EVENT_MQTT_CONNECTED:
        PR_INFO("Device MQTT Connected!");
#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL
        quaddle_ble_hid_central_set_wifi_busy(false);
#endif
        tal_event_publish(EVENT_MQTT_CONNECTED, NULL);

        static uint8_t first = 1;
        if (first) {
            first = 0;

#if defined(ENABLE_CHAT_DISPLAY) && (ENABLE_CHAT_DISPLAY == 1)
            UI_WIFI_STATUS_E wifi_status = UI_WIFI_STATUS_GOOD;
            ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&wifi_status, sizeof(UI_WIFI_STATUS_E));
#endif
            ai_audio_volume_upload();
        }
        break;

    /* MQTT with tuya cloud is disconnected, device offline */
    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_INFO("Device MQTT DisConnected!");
        tal_event_publish(EVENT_MQTT_DISCONNECTED, NULL);
        break;

    /* RECV upgrade request */
    case TUYA_EVENT_UPGRADE_NOTIFY:
        user_upgrade_notify_on(client, event->value.asJSON);
        break;

    /* Sync time with tuya Cloud */
    case TUYA_EVENT_TIMESTAMP_SYNC:
        PR_INFO("Sync timestamp:%d", event->value.asInteger);
        tal_time_set_posix(event->value.asInteger, 1);
        tal_event_publish("app.time.sync", NULL);
        break;

    case TUYA_EVENT_RESET:
        PR_INFO("Device Reset:%d", event->value.asInteger);
#if defined(ENABLE_QUADDLE_BLE_HID_CENTRAL) && ENABLE_QUADDLE_BLE_HID_CENTRAL
        quaddle_ble_hid_central_set_wifi_busy(false);
#endif

        _need_reset = 1;
        break;

    /* RECV OBJ DP */
    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        dp_obj_recv_t *dpobj = event->value.dpobj;
        bool           need_report = false;

        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d CNT:%u", dpobj->cmd_tp, dpobj->dtt_tp, dpobj->dpscnt);
        if (dpobj->devid != NULL) {
            PR_DEBUG("devid.%s", dpobj->devid);
        }

        audio_dp_obj_proc(dpobj, &need_report);
        if (need_report) {
            __dp_obj_report(client, dpobj);
        }

    } break;

    /* RECV RAW DP */
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

/**
 * @brief user defined network check callback, it will check the network every 1sec,
 *        in this demo it alwasy return ture due to it's a wired demo
 *
 * @return true
 * @return false
 */
bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status == NETMGR_LINK_DOWN ? false : true;
}

/* On ESP32 platforms, use heap_caps_* APIs for detailed heap fragmentation stats.
 * PLATFORM_ESP32 is the TuyaOpen-native macro (defined in tuya_kconfig.h) for all
 * ESP32 series chips. The Tuya CMake build does not add esp-idf heap component
 * headers to the include path, so we declare the required API prototypes directly;
 * they are resolved to the esp-idf implementation at link time. */
#ifdef PLATFORM_ESP32
extern size_t heap_caps_get_free_size(uint32_t caps);
extern size_t heap_caps_get_minimum_free_size(uint32_t caps);
extern size_t heap_caps_get_largest_free_block(uint32_t caps);

/* Capability bit for 8-bit-accessible DRAM, consistent with esp-idf esp_heap_caps.h */
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0x00000008U
#endif
#endif /* PLATFORM_ESP32 */

static void __printf_heap_tm_cb(TIMER_ID timer_id, void *arg)
{
#ifdef PLATFORM_ESP32
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_ever = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    PR_INFO("Heap: free=%-6u  min_ever=%-6u  largest_block=%-6u", (unsigned)free_now, (unsigned)min_ever,
            (unsigned)largest);
#else
    PR_INFO("Heap: free=%d", tal_system_get_free_heap_size());
#endif
}

void user_main(void)
{
    int ret = OPRT_OK;

    //! open iot development kit runtim init
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_psram_malloc, .free_fn = tal_psram_free});
#else
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
#endif

    tal_log_init(TAL_LOG_LEVEL_INFO, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

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
    uint8_t *value = NULL;
    size_t len = 0;
    if (tal_kv_get("ty_ai_chat_par", &value, &len) == OPRT_OK) {
        PR_NOTICE("ty_ai_chat_par: %s", value);
        tal_kv_free(value);
    } else {
        PR_NOTICE("ty_ai_chat_par not found");
    }

    tal_sw_timer_init();
    tal_sw_timer_create(__printf_heap_tm_cb, NULL, &sg_printf_heap_tm);
    tal_sw_timer_start(sg_printf_heap_tm, PRINTF_FREE_HEAP_TIME, TAL_TIMER_CYCLE);
    tal_workq_init();
    tal_time_service_init();
    tal_cli_init();
    tal_cli_cmd_register(sg_kv_cli_cmd, sizeof(sg_kv_cli_cmd) / sizeof(sg_kv_cli_cmd[0]));
    tuya_authorize_init();

    reset_netconfig_start();

    if (OPRT_OK != tuya_authorize_read(&license)) {
        license.uuid    = TUYA_OPENSDK_UUID;
        license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("Replace the TUYA_OPENSDK_UUID and TUYA_OPENSDK_AUTHKEY contents, otherwise the demo cannot work.\n \
                Visit https://platform.tuya.com/purchase/index?type=6 to get the open-sdk uuid and authkey.");
    }

    /* Initialize Tuya device configuration */
    ret = tuya_iot_init(&ai_client, &(const tuya_iot_config_t){
                                        .software_ver = PROJECT_VERSION,
                                        .productkey   = TUYA_PRODUCT_ID,
                                        .uuid         = license.uuid,
                                        .authkey      = license.authkey,
                                        // .firmware_key      = TUYA_DEVICE_FIRMWAREKEY,
                                        .event_handler = user_event_handler_on,
                                        .network_check = user_network_check,
                                    });
    assert(ret == OPRT_OK);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    // network init
    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
    netmgr_init(type);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG_PREPARE, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
#endif

    PR_DEBUG("tuya_iot_init success");

    ret = board_register_hardware();
    if (ret != OPRT_OK) {
        PR_ERR("board_register_hardware failed");
    }

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
    ret = ai_mcp_volume_changed_cb_set(__mcp_volume_changed_cb, NULL);
    if (ret != OPRT_OK) {
        PR_ERR("ai_mcp_volume_changed_cb_set failed:%d", ret);
    }
#endif

    ret = app_chat_bot_init();
    if (ret != OPRT_OK) {
        PR_ERR("app_chat_bot_init failed");
    }

#if defined(ENABLE_BATTERY) && (ENABLE_BATTERY == 1)
    ret = app_battery_init();
    if (ret != OPRT_OK) {
        PR_ERR("app_battery_init failed");
    }
#endif

    /* Start tuya iot task */
    tuya_iot_start(&ai_client);

    tkl_wifi_set_lp_mode(0, 0);

    reset_netconfig_check();

    for (;;) {
        /* Loop to receive packets, and handles client keepalive */
        tuya_iot_yield(&ai_client);
    }
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth   = 4096;
    thrd_param.priority     = 4;
    thrd_param.thrdname     = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
