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
