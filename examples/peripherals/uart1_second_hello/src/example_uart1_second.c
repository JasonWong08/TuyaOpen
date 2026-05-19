/**
 * @file example_uart1_second.c
 * @brief UART1 second-serial test: send "hello, world." every 1s, echo RX to log + TX.
 *
 * DNESP32S3_BOX / ESP32-S3 default: UART1 TX=GPIO17, RX=GPIO18 @ 115200.
 * USB debug log uses UART0 (GPIO43/44) — do not confuse with the 4-pin extension port.
 */

#include <string.h>

#include "tal_api.h"
#include "tal_uart.h"
#include "tkl_uart.h"
#include "tkl_output.h"

/***********************************************************
 * Adjust if your board routes the second 4-pin header elsewhere
 ***********************************************************/
#ifndef UART1_TEST_PORT
#define UART1_TEST_PORT TUYA_UART_NUM_1
#endif
#ifndef UART1_TEST_TX_PIN
#define UART1_TEST_TX_PIN 17
#endif
#ifndef UART1_TEST_RX_PIN
#define UART1_TEST_RX_PIN 18
#endif
#ifndef UART1_TEST_BAUD
#define UART1_TEST_BAUD 115200
#endif

#define UART1_HELLO_LINE     "hello, world.\r\n"
#define UART1_RX_BUF_SIZE      256
#define UART1_HELLO_INTERVAL_MS 1000

static char sg_rx_buf[UART1_RX_BUF_SIZE];

static OPERATE_RET __uart1_test_init(void)
{
    TAL_UART_CFG_T cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_buffer_size    = UART1_RX_BUF_SIZE;
    /* Non-blocking RX: O_BLOCK would block forever in tal_uart_read when line is idle */
    cfg.open_mode         = 0;
    cfg.base_cfg.baudrate = UART1_TEST_BAUD;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    cfg.base_cfg.flowctrl = TUYA_UART_FLOWCTRL_NONE;

    tkl_uart1_set_pins((int)UART1_TEST_TX_PIN, (int)UART1_TEST_RX_PIN);

    return tal_uart_init(UART1_TEST_PORT, &cfg);
}

static void __uart1_poll_rx(void)
{
    int avail;
    int n;

    avail = tal_uart_get_rx_data_size(UART1_TEST_PORT);
    if (avail <= 0) {
        return;
    }

    n = tal_uart_read(UART1_TEST_PORT, (uint8_t *)sg_rx_buf, sizeof(sg_rx_buf) - 1);
    if (n <= 0) {
        return;
    }
    sg_rx_buf[n] = '\0';
    PR_NOTICE("UART1 RX %d bytes: %s", n, sg_rx_buf);
    tal_uart_write(UART1_TEST_PORT, (const uint8_t *)"echo: ", 6);
    tal_uart_write(UART1_TEST_PORT, (const uint8_t *)sg_rx_buf, (uint32_t)n);
}

void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("======== UART1 second port test ========");
    PR_NOTICE("Board: %s  Chip: %s", PLATFORM_BOARD, PLATFORM_CHIP);
    PR_NOTICE("UART1 TX=GPIO%d  RX=GPIO%d  baud=%d", (int)UART1_TEST_TX_PIN, (int)UART1_TEST_RX_PIN,
              UART1_TEST_BAUD);
    PR_NOTICE("USB log = UART0; test output = UART1 (4-pin header)");
    PR_NOTICE("Wiring: board TX -> peer RX, board RX -> peer TX, GND common");

    rt = __uart1_test_init();
    if (rt != OPRT_OK) {
        PR_ERR("tal_uart_init(UART1) failed: %d", rt);
        return;
    }

    PR_NOTICE("UART1 init OK. Sending \"%s\" every %d ms", "hello, world.",
              UART1_HELLO_INTERVAL_MS);

    while (1) {
        int written = tal_uart_write(UART1_TEST_PORT, (const uint8_t *)UART1_HELLO_LINE,
                                   strlen(UART1_HELLO_LINE));
        if (written != (int)strlen(UART1_HELLO_LINE)) {
            PR_WARN("UART1 write hello: %d/%u", written, (unsigned)strlen(UART1_HELLO_LINE));
        } else {
            PR_NOTICE("UART1 TX hello, world.");
        }

        __uart1_poll_rx();
        tal_system_sleep(UART1_HELLO_INTERVAL_MS);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    user_main();
    while (1) {
        tal_system_sleep(500);
    }
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth   = 1024 * 4;
    thrd_param.priority     = THREAD_PRIO_1;
    thrd_param.thrdname     = "uart1_test";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
