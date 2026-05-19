/**
 * @file second_uart.c
 * @brief Second UART (UART1) to robot MCU via tal_uart; pins from tkl_uart (S3: GPIO17/18).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include <string.h>

#include "tal_api.h"
#include "tal_uart.h"

#include "second_uart.h"

#if defined(ENABLE_CHAT_BOT_ROBOT_SECOND_UART) && ENABLE_CHAT_BOT_ROBOT_SECOND_UART

#define ROBOT_UART_PORT TUYA_UART_NUM_1

#define SECOND_UART_LAST_MAX 128

static bool   s_initialized;
static char   s_last_cmd[SECOND_UART_LAST_MAX];
static size_t s_last_len;

static void __second_uart_log_tx(const uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        return;
    }
    PR_NOTICE("second_uart: TX UART%d GPIO%d/%d %u bytes: \"%.*s\"", (int)SECOND_UART_HW_PORT, (int)SECOND_UART_TX_PIN,
              (int)SECOND_UART_RX_PIN, (unsigned)length, (int)length, (const char *)data);
}

static bool __second_uart_same_as_last(const void *data, size_t len)
{
    return (s_last_len == len && len > 0 && memcmp(s_last_cmd, data, len) == 0);
}

static void __second_uart_remember(const void *data, size_t len)
{
    if (len >= SECOND_UART_LAST_MAX) {
        len = SECOND_UART_LAST_MAX - 1;
    }
    memcpy(s_last_cmd, data, len);
    s_last_cmd[len] = '\0';
    s_last_len      = len;
}

static OPERATE_RET __second_uart_ensure_init(void)
{
    if (s_initialized) {
        return OPRT_OK;
    }
    return second_uart_init();
}

OPERATE_RET second_uart_init(void)
{
    TAL_UART_CFG_T cfg;

    if (s_initialized) {
        return OPRT_OK;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_buffer_size    = SECOND_UART_BUF_SIZE;
    cfg.open_mode         = 0;
    cfg.base_cfg.baudrate = SECOND_UART_BAUD_RATE;
    cfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.base_cfg.flowctrl = TUYA_UART_FLOWCTRL_NONE;

    OPERATE_RET rt = tal_uart_init(ROBOT_UART_PORT, &cfg);
    if (rt == OPRT_INVALID_PARM) {
        PR_WARN("second_uart: tal_uart_init UART%d returned %d (port busy?)", (int)ROBOT_UART_PORT, rt);
        s_initialized = true;
        return OPRT_OK;
    }
    if (rt != OPRT_OK) {
        PR_ERR("second_uart: tal_uart_init UART%d failed %d", (int)ROBOT_UART_PORT, rt);
        return rt;
    }

    s_initialized = true;
    s_last_len    = 0;
    PR_NOTICE("second_uart: ready UART%d TX=GPIO%d RX=GPIO%d @ %d baud", (int)SECOND_UART_HW_PORT,
              (int)SECOND_UART_TX_PIN, (int)SECOND_UART_RX_PIN, SECOND_UART_BAUD_RATE);
    return OPRT_OK;
}

void second_uart_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    tal_uart_deinit(ROBOT_UART_PORT);
    s_initialized = false;
    s_last_len    = 0;
}

OPERATE_RET second_uart_send_data(const uint8_t *data, size_t length)
{
    int written;

    if (!data || length == 0) {
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = __second_uart_ensure_init();
    if (rt != OPRT_OK) {
        return rt;
    }

    if (__second_uart_same_as_last(data, length)) {
        PR_DEBUG("second_uart: skip duplicate (%u bytes)", (unsigned)length);
        return OPRT_OK;
    }

    written = tal_uart_write(ROBOT_UART_PORT, data, (uint32_t)length);
    if (written < 0 || (size_t)written != length) {
        PR_WARN("second_uart: wrote %d of %u", written, (unsigned)length);
        return OPRT_COM_ERROR;
    }

    __second_uart_remember(data, length);
    __second_uart_log_tx(data, length);
    return OPRT_OK;
}

OPERATE_RET second_uart_send_test(const char *cmd)
{
    s_last_len = 0;
    return second_uart_send_string(cmd ? cmd : "kbk 3");
}

OPERATE_RET second_uart_send_string(const char *str)
{
    char   buf[SECOND_UART_LAST_MAX];
    size_t n;

    if (!str) {
        return OPRT_INVALID_PARM;
    }

    n = strlen(str);
    if (n + 2 >= sizeof(buf)) {
        return OPRT_INVALID_PARM;
    }
    memcpy(buf, str, n);
    if (n == 0 || buf[n - 1] != '\n') {
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return second_uart_send_data((const uint8_t *)buf, n);
}

OPERATE_RET second_uart_send_line(const char *str)
{
    return second_uart_send_string(str);
}

OPERATE_RET second_uart_send_stand_up_command(void)
{
    return second_uart_send_string(ROBOT_STAND_UP_CMD);
}

OPERATE_RET second_uart_send_rest_command(void)
{
    return second_uart_send_string(ROBOT_REST_CMD);
}

#endif /* ENABLE_CHAT_BOT_ROBOT_SECOND_UART */
