/**
 * @file robot_uart_voice.h
 * @brief Map ASR text to robot UART commands when cloud MCP is not invoked.
 */
#ifndef ROBOT_UART_VOICE_H
#define ROBOT_UART_VOICE_H

#include "tuya_cloud_types.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

OPERATE_RET robot_uart_voice_init(void);

/** Call from app ASR event handler (AI_USER_EVT_ASR_OK). */
void robot_uart_voice_on_asr(const char *asr_text);

#endif

#endif /* ROBOT_UART_VOICE_H */
