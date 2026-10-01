#pragma once
#include <stdint.h>
#include "lvgl.h"

// Live hardware diagnostics. Every field is read from the real chip/board; nothing is estimated
// or simulated. The same snapshot feeds the on-screen Test Mode and a once-per-second
// "DIAG {json}" line on the serial console, which the browser preview can read over Web Serial.
//
// Hardware is sampled once a second by the diag task, never from the LVGL task: the battery
// read goes over the I2C bus the GT911 touch controller also lives on.
typedef struct {
    uint32_t    uptime_s;
    const char *reset_reason;
    const char *fw_version;      // git describe of the build, "-dirty" if built with local edits
    char        fw_elf[9];       // first 8 hex chars of the ELF SHA256 - unique to each build
    float       cpu_load_pct[2]; // per core over the last second; NAN if run-time stats are off
    uint32_t    heap_free;
    uint32_t    heap_min_free;
    uint32_t    heap_largest_block;
    uint32_t    psram_free;
    uint32_t    psram_total;
    float       chip_temp_c;     // NAN if the sensor failed
    float       battery_v;       // negative if the expander ADC read failed
    float       battery_v_min;   // range over the last DIAG_BATT_WINDOW_S readings;
    float       battery_v_max;   //   a wide swing usually means no battery is attached
    uint8_t     backlight_pct;
    int16_t     exio_pins;       // IO expander pin levels as read back; -1 if the read failed
    uint32_t    exio_recoveries; // times the expander had lost its outputs and was re-asserted
    uint32_t    lvgl_stuck_s;    // seconds the LVGL task has been unresponsive (0 = healthy)
    uint32_t    rgb_restarts;    // times the watchdog restarted the RGB scan-out
    float       panel_refresh_hz;
    // UI frame timing over the last second, measured around LVGL's display events.
    uint32_t    redraws_per_s;
    float       frame_avg_ms;    // 0 when nothing was redrawn
    float       frame_max_ms;
    float       tap_ms;          // touch press -> end of the next rendered frame, worst case; 0 if no tap
    uint32_t    touches;
    int16_t     last_touch_x;    // -1 until the first touch
    int16_t     last_touch_y;
} diag_snapshot_t;

#define DIAG_BATT_WINDOW_S 10

void diag_init(lv_display_t *disp);
// Copies the latest snapshot. Cheap and safe to call from the LVGL task - no hardware access.
void diag_take(diag_snapshot_t *out);
void diag_note_touch(int16_t x, int16_t y);
void diag_reset_touches(void);
// Starts the background task that samples the hardware and prints a DIAG line every second.
void diag_start_serial_stream(void);
