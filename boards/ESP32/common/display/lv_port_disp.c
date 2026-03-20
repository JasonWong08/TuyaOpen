/**
 * @file lv_port_disp.c
 *
 */

/*Copy this file as "lv_port_disp.c" and set this value to "1" to enable content*/
#if 1

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include "lv_port_disp.h"
#include "board_config.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#ifndef DISPLAY_LVGL_FULL_REFRESH
#define DISPLAY_LVGL_FULL_REFRESH 0
#endif

#if defined(BOARD_DISPLAY_TYPE) && (BOARD_DISPLAY_TYPE == DISPLAY_TYPE_LCD_SH8601)
#include "lcd_sh8601.h"
#endif
/*********************
 *      DEFINES
 *********************/
#define TAG "esp32_lvgl"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;
static bool                        s_disp_hw_ready;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_port_disp_init(char *device)
{
    (void)device;
    s_disp_hw_ready = false;
    s_panel_io      = NULL;
    s_panel         = NULL;

    if (0 != board_display_init()) {
        return;
    }

    s_panel_io = (esp_lcd_panel_io_handle_t)board_display_get_panel_io_handle();
    s_panel    = (esp_lcd_panel_handle_t)board_display_get_panel_handle();
    if (NULL == s_panel_io || NULL == s_panel) {
        return;
    }

    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

#if defined(BOARD_DISPLAY_TYPE) && (BOARD_DISPLAY_TYPE == DISPLAY_TYPE_LCD_SH8601)
#ifndef BOARD_LCD_DEFAULT_BRIGHTNESS
#define BOARD_LCD_DEFAULT_BRIGHTNESS 80
#endif
    lcd_sh8601_set_backlight(BOARD_LCD_DEFAULT_BRIGHTNESS);
#endif

    s_disp_hw_ready = true;
}

void lv_port_disp_register_to_lvgl(void)
{
    if (!s_disp_hw_ready || s_panel_io == NULL || s_panel == NULL) {
        ESP_LOGE(TAG, "Display hardware not ready");
        return;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .control_handle = NULL,
        .buffer_size = DISPLAY_BUFFER_SIZE,
        .double_buffer = false,
        .trans_size = 0,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = DISPLAY_MONOCHROME,
        .rotation =
            {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        .color_format = DISPLAY_COLOR_FORMAT,
        .flags =
            {
                .buff_dma = DISPLAY_BUFF_DMA,
                .buff_spiram = DISPLAY_BUFF_SPIRAM,
                .sw_rotate = 0,
                .swap_bytes = DISPLAY_SWAP_BYTES,
                .full_refresh = DISPLAY_LVGL_FULL_REFRESH,
                .direct_mode = 0,
            },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
    ESP_LOGI(TAG, "LVGL display added successfully");
}

static void disp_deinit(void)
{

}

void lv_port_disp_deinit(void)
{
    lv_display_delete(lv_disp_get_default());
    disp_deinit();
}
#endif