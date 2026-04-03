/**
 * @file board_config.h
 * @brief Hardware pin and peripheral configuration for DNESP32C3_BOX board.
 *
 * Target: ESP32-C3, ~400KB SRAM, no PSRAM, 16MB Flash
 * Audio:  ES8311 codec via I2S + I2C
 * Display: ST7789 SPI LCD 240x280 (with Y_GAP=20)
 *
 * Pin mapping derived from xiaozhi lichuang-c3-dev reference design.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
 * Audio - I2S (ES8311 codec)
 ***********************************************************/

#define I2S_INPUT_SAMPLE_RATE  (16000)
#define I2S_OUTPUT_SAMPLE_RATE (16000)

#define I2S_NUM    (0)
#define I2S_MCK_IO (13)
#define I2S_BCK_IO (12)
#define I2S_WS_IO  (8)
#define I2S_DO_IO  (7)  /* DAC -> speaker */
#define I2S_DI_IO  (10) /* ADC <- microphone */

#define AUDIO_CODEC_DMA_DESC_NUM  (3)
#define AUDIO_CODEC_DMA_FRAME_NUM (240)

/***********************************************************
 * Audio - I2C control bus (ES8311)
 ***********************************************************/

#define I2C_NUM    (0)
#define I2C_SCL_IO (1)
#define I2C_SDA_IO (0)

#define AUDIO_CODEC_ES8311_ADDR (0x30)

#define AUDIO_CODEC_PA_IO (-1)

/***********************************************************
 * Button
 ***********************************************************/

#define BOARD_BUTTON_PIN       TUYA_GPIO_NUM_9
#define BOARD_BUTTON_ACTIVE_LV TUYA_GPIO_LEVEL_LOW

/***********************************************************
 * Display - ST7789 SPI LCD 240x280
 ***********************************************************/

#define DISPLAY_TYPE_UNKNOWN        0
#define DISPLAY_TYPE_OLED_SSD1306   1
#define DISPLAY_TYPE_LCD_SH8601     2
#define DISPLAY_TYPE_LCD_ST7789_80  3
#define DISPLAY_TYPE_LCD_ST7789_SPI 4

#define BOARD_DISPLAY_TYPE DISPLAY_TYPE_LCD_ST7789_SPI

#define LCD_SCLK_PIN (3)
#define LCD_MOSI_PIN (4)
#define LCD_MISO_PIN (-1)
#define LCD_DC_PIN   (6)
#define LCD_CS_PIN   (5)

#define DISPLAY_BACKLIGHT_PIN           TUYA_GPIO_NUM_2
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

#define DISPLAY_ST7789_COLOR_INVERT 1

#define DISPLAY_WIDTH  (240)
#define DISPLAY_HEIGHT (280)

#define DISPLAY_BUFFER_SIZE       (DISPLAY_WIDTH * 4)
#define DISPLAY_LVGL_FULL_REFRESH 0

#define DISPLAY_ST7789_Y_GAP 20

#define DISPLAY_MONOCHROME false

#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define DISPLAY_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

#define DISPLAY_BUFF_SPIRAM 0
#define DISPLAY_BUFF_DMA    1

#define DISPLAY_SWAP_BYTES 1

/***********************************************************
 * UART0 - Debug / flashing port
 ***********************************************************/

#define UART_NUM0_TX_PIN (21)
#define UART_NUM0_RX_PIN (20)

/***********************************************************
 * Board display API
 ***********************************************************/

int board_display_init(void);

void *board_display_get_panel_io_handle(void);

void *board_display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CONFIG_H__ */
