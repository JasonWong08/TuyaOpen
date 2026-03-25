/**
 * @file board_config.h
 * @brief Hardware pin and peripheral configuration for PETOI_ESP32C3_CUBE board.
 *
 * Target: ESP32-C3, ~400KB SRAM, no PSRAM, 16MB Flash
 * Audio:  ES8311 codec via I2S + I2C
 * Display: ST7789 SPI (xiaozhi lichuang-c3-dev: 1.69\" physical 240×280 + Y offset 20).
 * This board file uses 240×240 if your module is square; match xiaozhi config.h if
 * you see cropping or wrong aspect.
 *
 * UART: xiaozhi moved robot UART from GPIO18/19 (USB D-/D+ conflict) to GPIO20/21.
 * Debug UART0 here: RX=20, TX=21 (see README in xiaozhi/main/boards/lichuang-c3-dev/).
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

#define I2S_NUM (0)
/* Hybrid fix:
 * keep PA pin aligned with xiaozhi (GPIO13), but fallback to the previous
 * non-conflict I2S mapping to avoid GPIO12 unusable warning on this board. */
#define I2S_MCK_IO (-1)
#define I2S_BCK_IO (11)
#define I2S_WS_IO  (8)
#define I2S_DO_IO  (7)  /* DAC -> speaker */
#define I2S_DI_IO  (10) /* ADC <- microphone */

/* Reduce DMA descriptors to lower peak heap usage on C3 */
#define AUDIO_CODEC_DMA_DESC_NUM  (3)
#define AUDIO_CODEC_DMA_FRAME_NUM (240)

/***********************************************************
 * Audio - I2C control bus (ES8311)
 ***********************************************************/

#define I2C_NUM    (0)
#define I2C_SCL_IO (1)
#define I2C_SDA_IO (0)

/* ES8311 I2C address as required by esp_codec_dev audio_codec_new_i2c_ctrl():
 * the library internally shifts right by 1 (device_address = addr >> 1),
 * so pass the 8-bit write address 0x30 to get the correct 7-bit address 0x18. */
#define AUDIO_CODEC_ES8311_ADDR (0x30)

/* External PA enable pin used on xiaozhi lichuang-c3-dev compatible boards */
#define AUDIO_CODEC_PA_IO (13)

/***********************************************************
 * Button
 ***********************************************************/

/* ESP32-C3 built-in BOOT button */
#define BOARD_BUTTON_PIN       TUYA_GPIO_NUM_9
#define BOARD_BUTTON_ACTIVE_LV TUYA_GPIO_LEVEL_LOW

/***********************************************************
 * Display - ST7789 SPI LCD 240x240
 ***********************************************************/

/* Display type selection (must match lcd_st7789_spi.c guard macro) */
#define DISPLAY_TYPE_UNKNOWN        0
#define DISPLAY_TYPE_OLED_SSD1306   1
#define DISPLAY_TYPE_LCD_SH8601     2
#define DISPLAY_TYPE_LCD_ST7789_80  3
#define DISPLAY_TYPE_LCD_ST7789_SPI 4

#define BOARD_DISPLAY_TYPE DISPLAY_TYPE_LCD_ST7789_SPI

#define LCD_SCLK_PIN (3)
#define LCD_MOSI_PIN (4)
#define LCD_MISO_PIN (-1) /* not used */
#define LCD_DC_PIN   (6)
#define LCD_CS_PIN   (5)

/* Backlight: GPIO2 (see petoi_esp32c3_cube.c board_display_init); active-low = ON when pin LOW */
#define DISPLAY_BACKLIGHT_PIN           TUYA_GPIO_NUM_2
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

/* ST7789 多数模组需颜色反转；旧代码误用 BACKLIGHT_OUTPUT_INVERT(true) 曾等价于 invert on */
#define DISPLAY_ST7789_COLOR_INVERT 1

#define DISPLAY_WIDTH  (240)
#define DISPLAY_HEIGHT (280)

/* Partial buffer: 4 lines ≈ 240*4*2 = 1.92 KB
 * full_refresh=1 要求整帧缓冲（240*240*2 = 115 KB）与 partial buffer 冲突，
 * esp_lvgl_port 会在 lvgl_port_add_disp_priv 内断言崩溃，必须保持 0。*/
#define DISPLAY_BUFFER_SIZE       (DISPLAY_WIDTH * 4)
#define DISPLAY_LVGL_FULL_REFRESH 0

/* ST7789 controller has 240x320 GRAM; the 240x280 physical panel is centred
 * with a 20-row top offset (GRAM rows 20-299 map to physical rows 0-279).
 * Setting Y_GAP=20 shifts all LVGL writes +20 in GRAM so they land exactly
 * on the visible LCD area.  Without this, the bottom 20 rows stay unwritten
 * (garbage/snow) and the top status bar is clipped off-screen. */
#define DISPLAY_ST7789_Y_GAP 20

#define DISPLAY_MONOCHROME false

/* Portrait, no swap/mirror for standard ST7789 wiring */
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

#define DISPLAY_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

/* No PSRAM on ESP32-C3; use DMA-capable internal SRAM for the LVGL buffer */
#define DISPLAY_BUFF_SPIRAM 0
#define DISPLAY_BUFF_DMA    1

#define DISPLAY_SWAP_BYTES 1

/***********************************************************
 * UART0 - Debug / flashing port
 * ESP32-C3 default UART0: TX=GPIO21, RX=GPIO20
 ***********************************************************/

#define UART_NUM0_TX_PIN (21)
#define UART_NUM0_RX_PIN (20)

/***********************************************************
 * Board display API (implemented in petoi_esp32c3_cube.c)
 ***********************************************************/

int board_display_init(void);

void *board_display_get_panel_io_handle(void);

void *board_display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CONFIG_H__ */
