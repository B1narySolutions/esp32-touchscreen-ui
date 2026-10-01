#pragma once
#include <stdint.h>
#include "lvgl.h"

// Live hardware diagnostics. Every field is read from the real chip/board; nothing is estimated
// or simulated. The same snapshot feeds the on-screen Test Mode and a once-per-second
// "DIAG {json}" line on the serial console, which the browser preview can read over Web Serial.
typedef struct {
    uint32_t    uptime_s;
    const char *reset_reason;
    uint32_t    heap_free;
    uint32_t    heap_min_free;
    uint32_t    heap_largest_block;
    uint32_t    psram_free;
    uint32_t    psram_total;
    float       chip_temp_c;     // NAN if the sensor failed
    float       battery_v;       // negative if the expander ADC read failed
    uint8_t     backlight_pct;
    uint32_t    redraws_per_s;
    uint32_t    touches;
    int16_t     last_touch_x;    // -1 until the first touch
    int16_t     last_touch_y;
} diag_snapshot_t;

void diag_init(lv_display_t *disp);
void diag_take(diag_snapshot_t *out);
void diag_note_touch(int16_t x, int16_t y);
void diag_reset_touches(void);
// Starts the background task that prints a DIAG line every second.
void diag_start_serial_stream(void);
