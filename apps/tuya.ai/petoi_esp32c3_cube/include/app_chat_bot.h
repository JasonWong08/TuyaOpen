/**
 * @file app_chat_bot.h
 * @brief app_chat_bot module is used to
 * @version 0.1
 * @date 2025-03-25
 */

#ifndef __APP_CHAT_BOT_H__
#define __APP_CHAT_BOT_H__

#include "tuya_cloud_types.h"
#include "ai_chat_main.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
/**
 * @brief Lightweight init before cloud activation.
 *
 * This stage only prepares minimal UI/status display and avoids heavy
 * audio/AI allocations so BLE netcfg + TLS can succeed on ESP32-C3.
 */
OPERATE_RET app_chat_bot_precloud_init(void);

/**
 * @brief Full AI/chat init after MQTT is connected.
 */
OPERATE_RET app_chat_bot_postcloud_init(void);

/**
 * @brief Backward-compatible one-shot init (precloud + postcloud).
 */
OPERATE_RET app_chat_bot_init(void);

/**
 * @brief Whether full chat/audio modules are initialized.
 */
bool app_chat_bot_is_ready(void);

/**
 * @brief Try to initialize lightweight UI/status path in offline/degraded mode.
 *
 * This is used when MQTT cannot be established (e.g. auth rejected) so the
 * device still has visible runtime status instead of a blank screen.
 *
 * @param min_heap_bytes minimum free heap required to start UI.
 * @param status_text optional status text; pass NULL to keep current status.
 */
OPERATE_RET app_chat_bot_try_recover_ui(uint32_t min_heap_bytes, const char *status_text);

/**
 * @brief Try to initialize offline audio path and play one local alert.
 *
 * This keeps voice prompts available when MQTT auth fails and full AI chat
 * has not entered ready state.
 */
OPERATE_RET app_chat_bot_try_recover_audio_alert(uint32_t min_heap_bytes, AI_AUDIO_ALERT_TYPE_E alert);

/**
 * @brief Release offline audio recovery resources proactively.
 *
 * Useful after bind voice prompt is played and device enters
 * WiFi/TLS/MQTT activation path, to return heap to cloud stack.
 */
OPERATE_RET app_chat_bot_release_offline_audio(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CHAT_BOT_H__ */
