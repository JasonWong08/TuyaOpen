# 问题：链接失败（`lcd_st7789_spi_*` 未定义引用）

## 现象

ESP32-C3 内层 ESP-IDF 链接阶段报错类似：

- `undefined reference to 'lcd_st7789_spi_init'`
- `undefined reference to 'lcd_st7789_spi_get_panel_io_handle'`
- `undefined reference to 'lcd_st7789_spi_get_panel_handle'`

调用点通常来自板级文件（例如 `boards/ESP32/PETOI_ESP32C3_CUBE/petoi_esp32c3_cube.c` 的 `board_display_init()` 等）。

## 根因

`boards/ESP32/common/lcd/lcd_st7789_spi.c` 内部函数实现被条件编译包裹：

```c
#if defined(BOARD_DISPLAY_TYPE) && (BOARD_DISPLAY_TYPE == DISPLAY_TYPE_LCD_ST7789_SPI)
// ... lcd_st7789_spi_init / get_panel_* ...
#endif
```

如果板级 `board_config.h` 没有定义：

- `DISPLAY_TYPE_LCD_ST7789_SPI`
- `BOARD_DISPLAY_TYPE` 且其值为 `DISPLAY_TYPE_LCD_ST7789_SPI`

那么 `lcd_st7789_spi.c` 会被编译成“空对象文件”，最终链接找不到符号。

## 修复

在目标板的 `board_config.h` 中补齐显示类型枚举宏，并设置：

- `BOARD_DISPLAY_TYPE DISPLAY_TYPE_LCD_ST7789_SPI`

涉及文件：

- `boards/ESP32/PETOI_ESP32C3_CUBE/board_config.h`

## 验证

- `tos.py build` 通过链接阶段
- `nm` 检查 `lcd_st7789_spi.c.obj` 可看到 `lcd_st7789_spi_init` 等全局符号（可选）

