#pragma once
#include "lvgl.h"

// Creates one read-mostly "knob" (a 270-degree lv_arc, never dragged directly).
// Tapping it is the ONLY interaction it exposes - it opens the vertical slider
// (ui_vertical_slider.h) via ui_main_on_knob_tapped(). fx_index/knob_index identify
// which value in g_knob_values[][] this instance is showing.
lv_obj_t *ui_knob_create(lv_obj_t *parent, uint8_t fx_index, uint8_t knob_index);

// Updates the arc + numeric readout in place (called after the slider changes
// the underlying value, so the panel reflects it without a full rebuild).
void ui_knob_set_value(lv_obj_t *knob, int32_t value);
