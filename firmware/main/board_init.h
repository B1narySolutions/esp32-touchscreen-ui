#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "driver/i2c_master.h"

#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

// Brings up the Waveshare ESP32-S3-Touch-LCD-7B: IO expander, GT911 touch, RGB panel,
// backlight, and the LVGL display/touch via esp_lvgl_adapter. Returns the display.
// After this returns, wrap any LVGL calls in board_lvgl_lock()/board_lvgl_unlock().
lv_display_t *board_init(void);

// The one I2C master bus on GPIO8/9 (GT911 touch, IO expander, and the H1 "I2C" header).
// Add devices to it; never create a second bus on those pins.
i2c_master_bus_handle_t board_i2c_bus(void);

bool board_lvgl_lock(void);
void board_lvgl_unlock(void);
bool board_lvgl_try_lock(int32_t timeout_ms);
// Restarts the RGB panel's scan-out DMA (used by the diag watchdog if the display stalls).
esp_err_t board_restart_rgb(void);

void board_set_backlight_pct(uint8_t pct);
uint8_t board_get_backlight_pct(void);

// Panel scan-out rate implied by the RGB pixel clock and porch timings. This is the hard
// ceiling on visible frames per second - LVGL can't show frames faster than the panel scans.
float board_panel_refresh_hz(void);

// Reads the IO expander's pin levels back. If the LCD-reset or touch-reset line has dropped (the expander resets independently of the ESP32), logs a warning and
// rewrites the expander's configuration. *recovered says whether that happened.
esp_err_t board_check_expander(uint8_t *pins, bool *recovered);

// Battery voltage from the expander ADC (Waveshare's 3:1 divider, 3.3V ref, 10-bit).
// Returns a negative value if the read failed.
float board_battery_volts(void);
