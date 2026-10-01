#pragma once
#include "lvgl.h"

// Creates one chip in the effects rail for effect definition index fx_index
// (see enum in ui_effects_data.h). The chip stores fx_index as its user_data so
// it keeps its identity no matter where it gets dragged to in the rail.
lv_obj_t *ui_effect_chip_create(lv_obj_t *parent, uint8_t fx_index);

// Repaints border/label/dot/state-text to match the current on/bypass + selected
// state for this effect. Call after any state change.
void ui_effect_chip_refresh(lv_obj_t *chip, bool is_on, bool is_selected);
