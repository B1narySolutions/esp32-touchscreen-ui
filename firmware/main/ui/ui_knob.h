#pragma once
#include "lvgl.h"

// Creates one read-mostly "knob" (a 270-degree lv_arc, never dragged directly).
// Tapping it is the ONLY interaction it exposes - it opens the vertical slider
// (ui_vertical_slider.h) via ui_main_on_knob_tapped(). fx_index/knob_index identify
// which value in g_knob_values[][] this instance is showing.
lv_obj_t *ui_knob_create(lv_obj_t *parent, uint8_t fx_index, uint8_t knob_index, int32_t diameter);

// Ring diameter for a panel showing knob_count knobs (matches the preview's knobSize()).
static inline int32_t ui_knob_diameter_for(uint8_t knob_count) {
    return knob_count <= 2 ? 148 : knob_count == 3 ? 132 : knob_count == 4 ? 118 : 104;
}

// Updates the arc + numeric readout in place (called after the slider changes
// the underlying value, so the panel reflects it without a full rebuild).
void ui_knob_set_value(lv_obj_t *knob, int32_t value);
