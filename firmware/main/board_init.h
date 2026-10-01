#pragma once
#include <stdint.h>
#include "lvgl.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

// Brings up the Waveshare ESP32-S3-Touch-LCD-7B: IO expander, GT911 touch, RGB panel,
// backlight, and the LVGL display/touch via esp_lvgl_adapter. Returns the display.
// After this returns, wrap any LVGL calls in board_lvgl_lock()/board_lvgl_unlock().
lv_display_t *board_init(void);

bool board_lvgl_lock(void);
void board_lvgl_unlock(void);

void board_set_backlight_pct(uint8_t pct);
uint8_t board_get_backlight_pct(void);

// Battery voltage from the expander ADC (Waveshare's 3:1 divider, 3.3V ref, 10-bit).
// Returns a negative value if the read failed.
float board_battery_volts(void);
