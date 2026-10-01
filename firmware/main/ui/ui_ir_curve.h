#pragma once
#include <stdint.h>
#include "lvgl.h"

// Frequency-response graph for the CAB panel (one instance). The curve is the approximate
// shape in g_ir_shapes[] shaped by the LOW CUT / HIGH CUT knobs - an illustration, not a
// measurement of the impulse response (see ui_effects_data.h).
lv_obj_t *ui_ir_curve_create(lv_obj_t *parent, int32_t w, int32_t h);
void ui_ir_curve_update(uint8_t ir_index, int32_t lowcut, int32_t highcut, lv_color_t color);
