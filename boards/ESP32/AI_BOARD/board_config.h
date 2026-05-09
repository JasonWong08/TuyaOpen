/**
 * @file board_config.h
 * @brief AI_BOARD hardware pin and peripheral configuration
 * @version 0.1
 * @date 2026-05-09
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#include "sdkconfig.h"
#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define I2S_INPUT_SAMPLE_RATE  (16000)
#define I2S_OUTPUT_SAMPLE_RATE (16000)

/* I2C port and GPIOs */
#define I2C_NUM    (0)
#define I2C_SCL_IO (13)
#define I2C_SDA_IO (14)

/* I2S port and GPIOs */
#define I2S_NUM    (0)
#define I2S_MCK_IO (12)
#define I2S_BCK_IO (11)
#define I2S_WS_IO  (9)
#define I2S_DO_IO  (8)
#define I2S_DI_IO  (10)

#define GPIO_OUTPUT_PA (-1)

#define AUDIO_CODEC_DMA_DESC_NUM  (6)
#define AUDIO_CODEC_DMA_FRAME_NUM (240)
#define AUDIO_CODEC_ES8311_ADDR   (0x30)

/*
 * Buttons:
 * - BOOT is available as a GPIO key (commonly GPIO0, active low)
 * - RESET is a chip reset pin, not used as an application button
 */
#ifndef BOARD_BUTTON_PIN
#define BOARD_BUTTON_PIN TUYA_GPIO_NUM_0
#endif

#ifndef BOARD_BUTTON_ACTIVE_LV
#define BOARD_BUTTON_ACTIVE_LV TUYA_GPIO_LEVEL_LOW
#endif

/*
 * UART0 pin mapping used by AI_BOARD:
 * TX -> GPIO35, RX -> GPIO36
 *
 * Note:
 * Current repository does not contain platform/ESP32/.../tkl_uart.c.
 * This board also exposes Kconfig options UART_NUM0_TX_PIN/UART_NUM0_RX_PIN
 * so the platform layer can consume these values when available.
 */
#ifndef UART_NUM0_TX_PIN
#define UART_NUM0_TX_PIN (35)
#endif

#ifndef UART_NUM0_RX_PIN
#define UART_NUM0_RX_PIN (36)
#endif

#if defined(CONFIG_AI_BOARD_ENABLE_LCD) && (CONFIG_AI_BOARD_ENABLE_LCD == 1)
#define AI_BOARD_ENABLE_LCD 1
#else
#define AI_BOARD_ENABLE_LCD 0
#endif

/* display */
#define DISPLAY_TYPE_UNKNOWN        0
#define DISPLAY_TYPE_OLED_SSD1306   1
#define DISPLAY_TYPE_LCD_SH8601     2
#define DISPLAY_TYPE_LCD_ST7789_80  3
#define DISPLAY_TYPE_LCD_ST7789_SPI 4

#if (AI_BOARD_ENABLE_LCD == 1)
/*
 * LCD pins are not finalized for AI_BOARD yet.
 * Keep placeholders here and update them when hardware is finalized.
 */
#define BOARD_DISPLAY_TYPE DISPLAY_TYPE_LCD_ST7789_SPI

#define LCD_SCLK_PIN (-1)
#define LCD_MOSI_PIN (-1)
#define LCD_MISO_PIN (-1)
#define LCD_DC_PIN   (-1)
#define LCD_CS_PIN   (-1)

#define DISPLAY_BACKLIGHT_PIN           (-1)
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_WIDTH  (240)
#define DISPLAY_HEIGHT (240)

/* lvgl config */
#define DISPLAY_BUFFER_SIZE (DISPLAY_WIDTH * 10)

#define DISPLAY_MONOCHROME false

/* rotation */
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define DISPLAY_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

/* Only one of DISPLAY_BUFF_SPIRAM and DISPLAY_BUFF_DMA can be selected */
#define DISPLAY_BUFF_SPIRAM 0
#define DISPLAY_BUFF_DMA    1

#define DISPLAY_SWAP_BYTES 1
#else
#define BOARD_DISPLAY_TYPE DISPLAY_TYPE_UNKNOWN
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
int board_display_init(void);

void *board_display_get_panel_io_handle(void);

void *board_display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CONFIG_H__ */
