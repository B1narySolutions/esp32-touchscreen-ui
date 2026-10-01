#include "ui_knob.h"
#include "ui_effects_data.h"
#include "ui_theme.h"
#include "ui_main.h"

// fx_index/knob_index packed into one pointer-sized user_data value
static void *pack(uint8_t fx, uint8_t k) { return (void *)(uintptr_t)((fx << 8) | k); }
static uint8_t unpack_fx(void *p) { return (uint8_t)(((uintptr_t)p >> 8) & 0xFF); }
static uint8_t unpack_knob(void *p) { return (uint8_t)((uintptr_t)p & 0xFF); }

static void knob_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *knob = lv_event_get_target(e);
    void *ud = lv_obj_get_user_data(knob);
    ui_main_on_knob_tapped(unpack_fx(ud), unpack_knob(ud));
}

lv_obj_t *ui_knob_create(lv_obj_t *parent, uint8_t fx_index, uint8_t knob_index, int32_t diameter) {
    const ui_effect_def_t *def = &g_effects[fx_index];
    const ui_knob_def_t *kd = &def->knobs[knob_index];
    int32_t value = g_knob_values[fx_index][knob_index];

    lv_obj_t *knob = lv_obj_create(parent);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, diameter + 28, diameter + 34);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(knob, pack(fx_index, knob_index));

    lv_obj_t *arc = lv_arc_create(knob);
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, 135, 45); // 270 degree sweep, gap at the bottom
    lv_arc_set_value(arc, value);
    lv_arc_set_rotation(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE); // never draggable; taps fall through to the knob button
    const int32_t ring = diameter >= 118 ? 12 : 10;
    lv_obj_set_style_arc_width(arc, ring, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ring, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, def->color, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB); // no draggable thumb nub

    lv_obj_t *value_label = lv_label_create(arc);
    lv_label_set_text_fmt(value_label, "%d%%", (int)value);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_16, 0);
    lv_obj_center(value_label);

    lv_obj_t *caption = lv_label_create(knob);
    lv_label_set_text(caption, kd->label);
    lv_obj_set_style_text_color(caption, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
    lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, 0);

    // The whole knob object is the tap target, so the hand can land anywhere in
    // the whole box, not just the ring stroke.
    lv_obj_add_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(knob, knob_event_cb, LV_EVENT_CLICKED, NULL);

    return knob;
}

void ui_knob_set_value(lv_obj_t *knob, int32_t value) {
    lv_obj_t *arc = lv_obj_get_child(knob, 0);
    lv_obj_t *value_label = lv_obj_get_child(arc, 0);
    lv_arc_set_value(arc, value);
    lv_label_set_text_fmt(value_label, "%d%%", (int)value);
}
