#pragma once
#include "lvgl.h"

// Builds the full 1024x600 screen (top bar, effects rail, fx panel, bottom bar,
// slider + settings overlays) and loads it. Call once after lv_init()/display init.
void ui_main_init(void);

// --- Callbacks other ui_* widgets call into; keeps ui_effect_chip.c / ui_knob.c /
//     ui_vertical_slider.c from needing to know about each other or hold app state.
void ui_main_on_chip_tapped(uint8_t fx_index);
void ui_main_on_chip_reordered(lv_obj_t *rail);  // reads the new order from the rail's chips
void ui_main_on_knob_tapped(uint8_t fx_index, uint8_t knob_index);
void ui_main_mark_dirty(void);

// The slider popup changed g_knob_values[fx_index][knob_index]: refresh that knob (and the CAB
// curve) on screen if visible, and mark the preset edited.
void ui_main_on_knob_changed(uint8_t fx_index, uint8_t knob_index);

// The effect whose panel is open (FX_* index). Safe to read from any task.
uint8_t ui_main_selected_fx(void);
