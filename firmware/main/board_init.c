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
#include "esp_heap_caps.h"
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
#define LCD_HPW       162
#define LCD_HBP       152
#define LCD_HFP       48
#define LCD_VPW       45
#define LCD_VBP       13
#define LCD_VFP       3

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_handle_t s_panel;
static uint8_t s_backlight_pct = 100; // full brightness; the panel reads dim below this

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
            .hsync_pulse_width = LCD_HPW,
            .hsync_back_porch = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VPW,
            .vsync_back_porch = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        // 30 lines (Waveshare uses 20): more slack before a late refill starves the panel.
        // Two of these live in internal RAM (~120 KB); must divide the 600-line frame evenly.
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 30,
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
    // DOUBLE_DIRECT: LVGL draws straight into the back frame buffer, then the buffers swap on
    // VSYNC and only the redrawn areas are copied across. Waveshare's default (TRIPLE_PARTIAL)
    // instead memcpy()s the whole un-redrawn screen (~1.2 MB PSRAM to PSRAM) after every frame,
    // because the S3 has no DMA2D: ~48 ms of a ~50 ms frame, pinning core 0 at ~98 % while a
    // finger is dragging. DOUBLE_DIRECT needs the IRAM-safe RGB ISR, 30-line bounce buffers
    // and restart-in-VSYNC (sdkconfig.defaults) - without them the panel shifted, showed
    // static, tore and sometimes went black. See docs/HANDOFF.md.
    const esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;

    init_i2c();
    ESP_ERROR_CHECK(io_expander_init(s_i2c_bus));

    esp_lcd_panel_handle_t panel = init_rgb_panel(esp_lv_adapter_get_required_frame_buffer_count(tear, rotation));
    s_panel = panel;
    esp_lcd_touch_handle_t touch = init_touch();

    io_expander_write(EXIO_BL_EN, 1);
    board_set_backlight_pct(s_backlight_pct);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 12 * 1024;
    adapter_cfg.stack_in_psram = true;
    // Core 0 services the RGB bounce-buffer interrupt (the panel is created from app_main
    // there); keep the LVGL task on the other core so the two don't contend.
    adapter_cfg.task_core_id = 1;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel, NULL, BOARD_LCD_H_RES, BOARD_LCD_V_RES, rotation);
    disp_cfg.profile.use_psram = true;
    disp_cfg.tear_avoid_mode = tear; // the macro hard-codes the RGB default; must match num_fbs above
    // With CONFIG_LCD_RGB_ISR_IRAM_SAFE the RGB driver requires the callback context (the
    // adapter's bridge struct) to be in internal RAM, but the adapter mallocs it and malloc
    // puts anything over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes in PSRAM - registration then
    // fails and LVGL waits forever for VSYNC. Keep plain mallocs internal just for this call.
    heap_caps_malloc_extmem_enable(64 * 1024);
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
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
bool board_lvgl_try_lock(int32_t timeout_ms) { return esp_lv_adapter_lock(timeout_ms) == ESP_OK; }
esp_err_t board_restart_rgb(void) { return s_panel ? esp_lcd_rgb_panel_restart(s_panel) : ESP_ERR_INVALID_STATE; }
void board_lvgl_unlock(void) { esp_lv_adapter_unlock(); }

void board_set_backlight_pct(uint8_t pct) {
    if (pct < 10) pct = 10; // never let the user black the screen out completely
    s_backlight_pct = pct;
    io_expander_set_backlight(pct);
}

uint8_t board_get_backlight_pct(void) { return s_backlight_pct; }

float board_panel_refresh_hz(void) {
    const float h_total = BOARD_LCD_H_RES + LCD_HPW + LCD_HBP + LCD_HFP;
    const float v_total = BOARD_LCD_V_RES + LCD_VPW + LCD_VBP + LCD_VFP;
    return LCD_PCLK_HZ / (h_total * v_total);
}

float board_battery_volts(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t raw = 0;
        if (io_expander_read_adc(&raw) != ESP_OK) return -1.0f;
        sum += raw;
    }
    return (sum / 4.0f) * 3.0f * 3.3f / 1023.0f;
}

esp_err_t board_check_expander(uint8_t *pins, bool *recovered) {
    // BL_EN is left out: that pin also carries the backlight PWM, so its read-back level follows
    // the PWM (always low at 100 % with the inverted duty), not the enable state.
    // USB_SEL is checked too: if it flipped to CAN, the console port would vanish.
    const uint8_t mask = (1u << EXIO_TP_RST) | (1u << EXIO_LCD_RST) | (1u << EXIO_USB_CAN);
    *recovered = false;
    esp_err_t err = io_expander_read_pins(pins);
    if (err != ESP_OK) return err;
    const uint8_t want = io_expander_expected();
    if ((*pins ^ want) & mask) {
        ESP_LOGW(TAG, "IO expander lost its outputs (pins 0x%02x, expected 0x%02x) - re-asserting",
                 *pins, want);
        io_expander_reassert();
        *recovered = true;
    }
    return ESP_OK;
}
