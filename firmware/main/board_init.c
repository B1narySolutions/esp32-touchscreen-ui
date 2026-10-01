/*
 * Waveshare ESP32-S3-Touch-LCD-7B bring-up.
 *
 * Pins, RGB timings, the touch reset sequence and the IO-expander register map all come from
 * Waveshare's official ESP-IDF example for this exact board:
 *   https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B  (examples/ESP-IDF/17_lvgl_v9_demo)
 * The LVGL wiring uses the same esp_lvgl_adapter component that example uses.
 */
#include "board_init.h"
#include "io_expander.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define PIN_I2C_SDA   GPIO_NUM_8
#define PIN_I2C_SCL   GPIO_NUM_9
#define PIN_TP_INT    GPIO_NUM_4

#define PIN_HSYNC     GPIO_NUM_46
#define PIN_VSYNC     GPIO_NUM_3
#define PIN_DE        GPIO_NUM_5
#define PIN_PCLK      GPIO_NUM_7

#define LCD_PCLK_HZ   (24 * 1000 * 1000)

static i2c_master_bus_handle_t s_i2c_bus;
static uint8_t s_backlight_pct = 80;

static void init_i2c(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c_bus));
}

static esp_lcd_panel_handle_t init_rgb_panel(uint8_t num_fbs) {
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 45,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 20,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            14, 38, 18, 17, 10,     // D0-D4: blue
            39, 0, 45, 48, 47, 21,  // D5-D10: green
            1, 2, 42, 41, 40,       // D11-D15: red
        },
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

// GT911 picks its I2C address from the INT pin level while RST rises; this is
// Waveshare's exact sequence for this board (RST lives on the IO expander).
static esp_lcd_touch_handle_t init_touch(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    io_expander_write(EXIO_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_TP_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    io_expander_write(EXIO_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = PIN_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed (%s) - continuing without touch", esp_err_to_name(err));
        return NULL;
    }
    return touch;
}

lv_display_t *board_init(void) {
    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    init_i2c();
    ESP_ERROR_CHECK(io_expander_init(s_i2c_bus));

    esp_lcd_panel_handle_t panel = init_rgb_panel(esp_lv_adapter_get_required_frame_buffer_count(tear, rotation));
    esp_lcd_touch_handle_t touch = init_touch();

    io_expander_write(EXIO_BL_EN, 1);
    board_set_backlight_pct(s_backlight_pct);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 12 * 1024;
    adapter_cfg.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel, NULL, BOARD_LCD_H_RES, BOARD_LCD_V_RES, rotation);
    disp_cfg.profile.use_psram = true;
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(disp != NULL);

    if (touch) {
        esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);
        assert(esp_lv_adapter_register_touch(&touch_cfg) != NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    ESP_LOGI(TAG, "display %dx%d up, touch %s", BOARD_LCD_H_RES, BOARD_LCD_V_RES, touch ? "ok" : "MISSING");
    return disp;
}

bool board_lvgl_lock(void) { return esp_lv_adapter_lock(-1) == ESP_OK; }
void board_lvgl_unlock(void) { esp_lv_adapter_unlock(); }

void board_set_backlight_pct(uint8_t pct) {
    if (pct < 10) pct = 10; // never let the user black the screen out completely
    s_backlight_pct = pct;
    io_expander_set_backlight(pct);
}

uint8_t board_get_backlight_pct(void) { return s_backlight_pct; }

float board_battery_volts(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t raw = 0;
        if (io_expander_read_adc(&raw) != ESP_OK) return -1.0f;
        sum += raw;
    }
    return (sum / 4.0f) * 3.0f * 3.3f / 1023.0f;
}
