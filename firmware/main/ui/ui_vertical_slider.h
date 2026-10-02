#pragma once
#include "lvgl.h"

// The one shared "multipurpose slider" popup. Created once, hidden, and re-targeted
// at whatever knob was last tapped. This is the sponsor's core requirement: no knob
// is ever adjusted by dragging it directly - every adjustment funnels through here.
void ui_vertical_slider_init(lv_obj_t *screen);

// Shows the popup pre-loaded for one knob and dims everything behind it. Tapping
// the backdrop (or the X button) closes it; nothing else on screen is reachable
// while it's open.
void ui_vertical_slider_show(uint8_t fx_index, uint8_t knob_index);
void ui_vertical_slider_hide(void);

// The parameter the popup is editing, for a physical knob to follow. Safe from any task (reads
// two bytes and a flag the LVGL task keeps). Returns false while the popup is closed.
bool ui_vertical_slider_target(uint8_t *fx_index, uint8_t *knob_index);
// Re-reads the target's value after something other than the popup changed it (LVGL task only).
void ui_vertical_slider_refresh(void);
