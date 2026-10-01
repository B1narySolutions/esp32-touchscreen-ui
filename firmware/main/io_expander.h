#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"

// On-board IO expander of the Waveshare ESP32-S3-Touch-LCD-7B (I2C address 0x24).
// Register map taken from Waveshare's own 7B ESP-IDF examples (io_extension.c).
#define EXIO_TP_RST   1
#define EXIO_BL_EN    2
#define EXIO_LCD_RST  3
#define EXIO_SD_CS    4
#define EXIO_USB_CAN  5

esp_err_t io_expander_init(i2c_master_bus_handle_t bus);
esp_err_t io_expander_write(uint8_t pin, uint8_t level);
// Backlight brightness, 0-100. The expander's PWM output is inverted on this board.
esp_err_t io_expander_set_backlight(uint8_t pct);
// Raw 10-bit reading of the battery sense divider; see board_battery_volts().
esp_err_t io_expander_read_adc(uint16_t *out);
