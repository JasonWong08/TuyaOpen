/**
 * @file robot_uart_voice.h
 * @brief Map ASR text to robot UART commands when cloud MCP is not invoked.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef ROBOT_UART_VOICE_H
#define ROBOT_UART_VOICE_H

#include "tuya_cloud_types.h"
#include "ai_user_event.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

OPERATE_RET robot_uart_voice_init(void);

/** Call from app ASR event handler (AI_USER_EVT_ASR_OK). */
void robot_uart_voice_on_asr(const char *asr_text);

/** Map TTS start/end to second-UART listen/talk cues. */
void robot_uart_voice_on_event(const AI_NOTIFY_EVENT_T *event);

#endif

#endif /* ROBOT_UART_VOICE_H */
