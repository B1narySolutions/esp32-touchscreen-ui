#pragma once
#include "lvgl.h"

// Full-screen diagnostics overlay: live ESP32-S3 readings, SEED3 link status, a touch test pad
// and a color-bar panel test. Built once (hidden); opened from the side menu.
void ui_test_mode_init(lv_obj_t *screen);
void ui_test_mode_open(void);
