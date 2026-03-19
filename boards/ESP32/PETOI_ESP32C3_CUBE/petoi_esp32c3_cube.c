/**
 * @file petoi_esp32c3_cube.c
 * @brief Board-level hardware initialization for PETOI_ESP32C3_CUBE.
 *
 * Target: ESP32-C3, ~400 KB SRAM, no PSRAM, 16 MB Flash.
 *
 * Peripherals:
 *   - ES8311 audio codec (I2S + I2C)
 *   - ST7789 SPI LCD 240×240
 *   - BOOT button (GPIO9, active-low)
 *
 * Memory notes:
 *   - DMA descriptor count is kept small (3) to reduce peak heap pressure.
 *   - LCD LVGL buffer is 10 lines (240×10×2 = 4.8 KB) in DMA-capable SRAM.
 *   - No PSRAM branches; all allocations use internal heap.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tuya_cloud_types.h"
#include "board_config.h"
#include "board_com_api.h"

#include "tal_api.h"

#include "tdd_audio_8311_codec.h"

#if defined(ENABLE_BUTTON) && (ENABLE_BUTTON == 1)
#include "tdd_button_gpio.h"
#endif

#include "lcd_st7789_spi.h"

/***********************************************************
 * Internal helpers
 ***********************************************************/

static OPERATE_RET __board_register_audio(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(AUDIO_CODEC_NAME)
    TDD_AUDIO_8311_CODEC_T cfg = {
        .i2c_id         = I2C_NUM,
        .i2c_sda_io     = I2C_SDA_IO,
        .i2c_scl_io     = I2C_SCL_IO,
        .mic_sample_rate = I2S_INPUT_SAMPLE_RATE,
        .spk_sample_rate = I2S_OUTPUT_SAMPLE_RATE,
        .i2s_id         = I2S_NUM,
        .i2s_mck_io     = I2S_MCK_IO,
        .i2s_bck_io     = I2S_BCK_IO,
        .i2s_ws_io      = I2S_WS_IO,
        .i2s_do_io      = I2S_DO_IO,
        .i2s_di_io      = I2S_DI_IO,
        .gpio_output_pa = AUDIO_CODEC_PA_IO,   /* NC: no external PA enable */
        .es8311_addr    = AUDIO_CODEC_ES8311_ADDR,
        .dma_desc_num   = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num  = AUDIO_CODEC_DMA_FRAME_NUM,
        .default_volume = 80,
    };

    TUYA_CALL_ERR_RETURN(tdd_audio_8311_codec_register(AUDIO_CODEC_NAME, cfg));
#endif

    return rt;
}

static OPERATE_RET __board_register_button(void)
{
#if !defined(ENABLE_BUTTON) || (ENABLE_BUTTON != 1)
    return OPRT_OK;
#else
    OPERATE_RET rt = OPRT_OK;

#if defined(BUTTON_NAME)
    BUTTON_GPIO_CFG_T button_hw_cfg = {
        .pin    = BOARD_BUTTON_PIN,
        .level  = BOARD_BUTTON_ACTIVE_LV,
        .mode   = BUTTON_TIMER_SCAN_MODE,
        .pin_type.gpio_pull = TUYA_GPIO_PULLUP,
    };

    TUYA_CALL_ERR_RETURN(tdd_gpio_button_register(BUTTON_NAME, &button_hw_cfg));
#endif

    return rt;
#endif
}

/***********************************************************
 * Public API
 ***********************************************************/

/**
 * @brief Register all hardware peripherals on the PETOI_ESP32C3_CUBE board.
 *
 * Initialization order:
 *   1. Button  – lightweight, no heap impact
 *   2. Audio   – I2C + I2S buses, ES8311 codec
 *
 * The LCD is initialised separately via board_display_init(), called by the
 * display subsystem only when a UI is actually needed (on-demand strategy).
 */
OPERATE_RET board_register_hardware(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_LOG(__board_register_button());
    TUYA_CALL_ERR_LOG(__board_register_audio());

    return rt;
}

/***********************************************************
 * Display API  (ST7789 SPI, called on-demand by UI layer)
 ***********************************************************/

int board_display_init(void)
{
    return lcd_st7789_spi_init();
}

void *board_display_get_panel_io_handle(void)
{
    return lcd_st7789_spi_get_panel_io_handle();
}

void *board_display_get_panel_handle(void)
{
    return lcd_st7789_spi_get_panel_handle();
}
