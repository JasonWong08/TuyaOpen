/**
 * @file second_uart.h
 * @brief Second UART to lower MCU (robot dog / motion board), UART1 on ESP32-S3 DNE BOX.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Logic mirrors xiaozhi second_uart: init, dedupe, stand-up/rest helpers, raw send.
 */
#ifndef YOUR_CHAT_BOT_SECOND_UART_H
#define YOUR_CHAT_BOT_SECOND_UART_H

#include <stddef.h>

#include "tuya_cloud_types.h"

/* Override before init if your 4-pin header is not GPIO17/18 (DNESP32S3_BOX default). */
#ifndef SECOND_UART_TX_PIN
#define SECOND_UART_TX_PIN (17)
#endif
#ifndef SECOND_UART_RX_PIN
#define SECOND_UART_RX_PIN (18)
#endif

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#ifdef __cplusplus
extern "C" {
#endif

/* UART0 uses GPIO43/44 for logs; UART1 drives the external TTL header. */
#ifndef SECOND_UART_HW_PORT
#define SECOND_UART_HW_PORT (1)
#endif
#ifndef SECOND_UART_BAUD_RATE
#define SECOND_UART_BAUD_RATE (115200)
#endif
#ifndef SECOND_UART_BUF_SIZE
#define SECOND_UART_BUF_SIZE (1024)
#endif

#define ROBOT_STAND_UP_CMD "kup"
#define ROBOT_REST_CMD     "d"

OPERATE_RET second_uart_init(void);
void        second_uart_deinit(void);

OPERATE_RET second_uart_send_data(const uint8_t *data, size_t length);
OPERATE_RET second_uart_send_string(const char *str);
OPERATE_RET second_uart_send_line(const char *str);

OPERATE_RET second_uart_send_stand_up_command(void);
OPERATE_RET second_uart_send_rest_command(void);

/** Manual test: send raw string (adds newline). For CLI / debugging. */
OPERATE_RET second_uart_send_test(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */

#endif /* YOUR_CHAT_BOT_SECOND_UART_H */
