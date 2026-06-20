/**
 * @file quaddle_robot_bridge.h
 * @brief Quaddle gamepad event to robot UART command bridge.
 *
 * The BLE/HID layer should feed decoded text lines compatible with the Arduino
 * QuaddleGame output, for example "[Q34B] A+" or "[BM769] LSTICK 20,-3".
 */
#ifndef YOUR_CHAT_BOT_QUADDLE_ROBOT_BRIDGE_H
#define YOUR_CHAT_BOT_QUADDLE_ROBOT_BRIDGE_H

#include "tuya_cloud_types.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET quaddle_robot_bridge_init(void);
void        quaddle_robot_bridge_reset(void);
void        quaddle_robot_bridge_poll(void);
OPERATE_RET quaddle_robot_bridge_handle_line(const char *line);
OPERATE_RET quaddle_robot_bridge_queue_ai_command(const char *cmd, const char *source);
BOOL_T      quaddle_robot_bridge_gamepad_active(void);
uint32_t    quaddle_robot_bridge_gamepad_active_remaining_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */

#endif /* YOUR_CHAT_BOT_QUADDLE_ROBOT_BRIDGE_H */
