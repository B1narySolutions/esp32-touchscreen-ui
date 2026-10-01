#include "ui_vertical_slider.h"
#include "ui_effects_data.h"
#include "ui_theme.h"
#include "ui_main.h"

static lv_obj_t *s_backdrop;
static lv_obj_t *s_card;
static lv_obj_t *s_label;
static lv_obj_t *s_value_label;
static lv_obj_t *s_slider;
static uint8_t s_target_fx;
static uint8_t s_target_knob;

static void apply_value(int32_t value) {
    value = value < 0 ? 0 : (value > 100 ? 100 : value);
    g_knob_values[s_target_fx][s_target_knob] = value;
    lv_label_set_text_fmt(s_value_label, "%d%%", (int)value);
    lv_slider_set_value(s_slider, value, LV_ANIM_OFF);
    ui_main_on_knob_changed(s_target_fx, s_target_knob);
}

static void backdrop_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_vertical_slider_hide();
}

static void close_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_vertical_slider_hide();
}

static void slider_value_changed_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    apply_value(lv_slider_get_value(s_slider));
}

static void minus_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        apply_value(g_knob_values[s_target_fx][s_target_knob] - 1);
}

static void plus_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        apply_value(g_knob_values[s_target_fx][s_target_knob] + 1);
}

void ui_vertical_slider_init(lv_obj_t *screen) {
    s_backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_backdrop, UI_COLOR_BACKDROP, 0);
    lv_obj_set_style_bg_opa(s_backdrop, 160, 0);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_backdrop, backdrop_event_cb, LV_EVENT_CLICKED, NULL);

    // Fixed-size card: pad 22 + label 16 + value 27 + slider 220 + buttons 44 + hint 15 + pad 22,
    // plus four 14px row gaps = 422, inside a 430 card. Never scrollable - if the
    // content grows, grow the card rather than let the popup scroll under a finger.
    s_card = lv_obj_create(s_backdrop);
    lv_obj_set_size(s_card, 220, 430);
    lv_obj_center(s_card);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_card, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(s_card, 22, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, UI_COLOR_BORDER, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_card, 14, 0);
    lv_obj_set_style_pad_ver(s_card, 22, 0);
    lv_obj_set_style_pad_hor(s_card, 16, 0);
    // Card must itself be clickable: LVGL's hit-test falls through a non-clickable
    // container to its clickable parent (the backdrop) wherever none of the card's
    // own children claim the point, which would close the popup on a tap anywhere
    // on the card that isn't a button/slider. Making the card clickable (with no
    // handler of its own) absorbs that tap instead. Events don't bubble back up to
    // the backdrop either way - LVGL only bubbles when a child sets
    // LV_OBJ_FLAG_EVENT_BUBBLE, which we never do here.
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close_btn = lv_button_create(s_card);
    lv_obj_set_size(close_btn, 40, 40);
    // Floating: taken out of the flex column (where lv_obj_align is ignored), so it
    // sits in the top-right corner instead of stacking above the label.
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 8, -14);
    lv_obj_set_style_bg_opa(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_center(close_label);

    s_label = lv_label_create(s_card);
    lv_obj_set_style_text_color(s_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);

    s_value_label = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_24, 0);

    s_slider = lv_slider_create(s_card);
    lv_slider_set_range(s_slider, 0, 100);
    lv_obj_set_size(s_slider, 58, 220);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_slider, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *btn_row = lv_obj_create(s_card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 14, 0);

    lv_obj_t *minus_btn = lv_button_create(btn_row);
    lv_obj_set_size(minus_btn, 44, 44);
    lv_obj_set_style_radius(minus_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(minus_btn, UI_COLOR_DIVIDER, 0);
    lv_obj_add_event_cb(minus_btn, minus_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_label = lv_label_create(minus_btn);
    lv_label_set_text(minus_label, "-");
    lv_obj_center(minus_label);

    lv_obj_t *plus_btn = lv_button_create(btn_row);
    lv_obj_set_size(plus_btn, 44, 44);
    lv_obj_set_style_radius(plus_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(plus_btn, UI_COLOR_DIVIDER, 0);
    lv_obj_add_event_cb(plus_btn, plus_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_label = lv_label_create(plus_btn);
    lv_label_set_text(plus_label, "+");
    lv_obj_center(plus_label);

    lv_obj_t *hint = lv_label_create(s_card);
    lv_label_set_text(hint, "tap outside to close");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_MUTED, 0);
}

void ui_vertical_slider_show(uint8_t fx_index, uint8_t knob_index) {
    const ui_effect_def_t *def = &g_effects[fx_index];
    const ui_knob_def_t *kd = &def->knobs[knob_index];
    int32_t value = g_knob_values[fx_index][knob_index];

    s_target_fx = fx_index;
    s_target_knob = knob_index;

    lv_label_set_text(s_label, kd->label);
    lv_label_set_text_fmt(s_value_label, "%d%%", (int)value);
    lv_obj_set_style_text_color(s_value_label, def->color, 0);
    lv_slider_set_value(s_slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, def->color, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(s_slider, def->color, LV_PART_KNOB);

    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void ui_vertical_slider_hide(void) {
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
}
