#include "ui_main.h"
#include "ui_theme.h"
#include "ui_effects_data.h"
#include "ui_effect_chip.h"
#include "ui_knob.h"
#include "ui_vertical_slider.h"
#include "ui_test_mode.h"
#include "board_init.h"
#include <string.h>
#include <stdio.h>

static uint8_t s_chain_order[UI_EFFECT_COUNT] = { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_CHORUS, FX_DELAY, FX_REVERB };
static bool s_effect_on[UI_EFFECT_COUNT]      = { false,   false,   false,    true,   false,     false,   false };
static uint8_t s_selected_fx = FX_AMP;
static bool s_dirty = false;
static bool s_muted = false;
static int32_t s_master_volume = 63;
static int32_t s_backlight = 80;
static bool s_wifi_on = true;
static bool s_ble_on = true;
static char s_preset_name[32] = "Preset 1";
static lv_obj_t *s_drawer_backdrop;

static lv_obj_t *s_chips[UI_EFFECT_COUNT]; // indexed by fx_index, NOT chain position
static lv_obj_t *s_chain_rail;
static lv_obj_t *s_fx_panel_title;
static lv_obj_t *s_fx_swatch;
static lv_obj_t *s_fx_model_dropdown;
static lv_obj_t *s_knob_row;
static lv_obj_t *s_preset_label;
static lv_obj_t *s_edited_badge;
static lv_obj_t *s_master_slider;
static lv_obj_t *s_mute_btn;
static lv_obj_t *s_mute_label;
static lv_obj_t *s_settings_backdrop;
static lv_obj_t *s_backlight_slider;
static lv_obj_t *s_wifi_switch, *s_wifi_knob;
static lv_obj_t *s_ble_switch, *s_ble_knob;

static void refresh_all_chip_styles(void) {
    for (uint8_t fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        ui_effect_chip_refresh(s_chips[fx], s_effect_on[fx], s_selected_fx == fx);
    }
}

static void rebuild_panel(void) {
    const ui_effect_def_t *def = &g_effects[s_selected_fx];

    lv_label_set_text(s_fx_panel_title, def->label);
    lv_obj_set_style_bg_color(s_fx_swatch, def->color, 0);

    if (def->model_key) {
        static char opts[256];
        opts[0] = '\0';
        for (uint8_t i = 0; def->model_options[i] != NULL; i++) {
            if (i > 0) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
            strncat(opts, def->model_options[i], sizeof(opts) - strlen(opts) - 1);
        }
        lv_dropdown_set_options(s_fx_model_dropdown, opts);
        lv_obj_clear_flag(s_fx_model_dropdown, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_fx_model_dropdown, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clean(s_knob_row);
    for (uint8_t k = 0; k < def->knob_count; k++) {
        ui_knob_create(s_knob_row, s_selected_fx, k);
    }
}

void ui_main_on_chip_tapped(uint8_t fx_index) {
    if (s_selected_fx != fx_index) {
        s_selected_fx = fx_index;
        rebuild_panel();
        refresh_all_chip_styles();
        return;
    }
    // Already the open chip: second tap bypasses, third tap re-enables, and so on.
    s_effect_on[fx_index] = !s_effect_on[fx_index];
    refresh_all_chip_styles();
    ui_main_mark_dirty();
    // TODO: tell the audio engine this effect is now on/bypassed in the signal path.
}

void ui_main_on_chip_reordered(lv_obj_t *rail) {
    uint32_t n = lv_obj_get_child_cnt(rail);
    for (uint32_t i = 0; i < n && i < UI_EFFECT_COUNT; i++) {
        s_chain_order[i] = (uint8_t)(uintptr_t)lv_obj_get_user_data(lv_obj_get_child(rail, i));
    }
    ui_main_mark_dirty();
    // TODO: tell the audio engine the new processing order (s_chain_order[0..6]).
}

void ui_main_on_knob_tapped(uint8_t fx_index, uint8_t knob_index) {
    ui_vertical_slider_show(fx_index, knob_index);
}

lv_obj_t *ui_main_find_visible_knob(uint8_t fx_index, uint8_t knob_index) {
    if (s_selected_fx != fx_index) return NULL;
    if (knob_index >= lv_obj_get_child_cnt(s_knob_row)) return NULL;
    return lv_obj_get_child(s_knob_row, knob_index);
}

void ui_main_mark_dirty(void) {
    s_dirty = true;
    lv_obj_clear_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);
}

static void save_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_dirty = false;
    lv_obj_add_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);
    // TODO: persist s_chain_order / s_effect_on / g_knob_values / model picks to NVS.
}

static void presets_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_clear_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void drawer_backdrop_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void test_mode_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
    ui_test_mode_open();
}

static void master_slider_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    s_master_volume = lv_slider_get_value(s_master_slider);
    s_muted = false;
    lv_label_set_text(s_mute_label, "MUTE");
    lv_obj_set_style_bg_color(s_mute_btn, UI_COLOR_CARD, 0);
    // TODO: apply s_master_volume to the real output gain stage.
}

static void mute_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_muted = !s_muted;
    lv_obj_set_style_bg_color(s_mute_btn, s_muted ? UI_COLOR_MUTE : UI_COLOR_CARD, 0);
    lv_slider_set_value(s_master_slider, s_muted ? 0 : s_master_volume, LV_ANIM_ON);
    // TODO: hard-mute the real output stage; s_master_volume itself is left untouched
    // so un-muting restores the same level.
}

static void settings_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_clear_flag(s_settings_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void settings_backdrop_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) lv_obj_add_flag(s_settings_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void backlight_slider_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    s_backlight = lv_slider_get_value(s_backlight_slider);
    board_set_backlight_pct((uint8_t)s_backlight);
}

static void toggle_switch(lv_obj_t *sw, lv_obj_t *knob, bool *state) {
    *state = !*state;
    lv_obj_set_style_bg_color(sw, *state ? UI_COLOR_ACCENT : lv_color_hex(0x2c2c33), 0);
    lv_obj_align(knob, *state ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, *state ? -3 : 3, 3);
}

static void wifi_switch_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) toggle_switch(s_wifi_switch, s_wifi_knob, &s_wifi_on);
    // TODO: actually enable/disable the Wi-Fi radio.
}
static void ble_switch_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) toggle_switch(s_ble_switch, s_ble_knob, &s_ble_on);
    // TODO: actually enable/disable the BLE stack.
}

static lv_obj_t *make_pill_switch(lv_obj_t *parent, bool initial, lv_event_cb_t cb, lv_obj_t **out_knob) {
    lv_obj_t *sw = lv_obj_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 46, 26);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sw, initial ? UI_COLOR_ACCENT : lv_color_hex(0x2c2c33), 0);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 20, 20);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_align(knob, initial ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, initial ? -3 : 3, 3);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);

    *out_knob = knob;
    return sw;
}

static void build_top_bar(lv_obj_t *screen) {
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 60);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 28, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preset_tag = lv_label_create(bar);
    lv_label_set_text(preset_tag, "PRESET");
    lv_obj_set_style_text_color(preset_tag, lv_color_hex(0x78746c), 0);
    lv_obj_align(preset_tag, LV_ALIGN_LEFT_MID, 0, 0);

    s_preset_label = lv_label_create(bar);
    lv_label_set_text(s_preset_label, s_preset_name);
    lv_obj_align(s_preset_label, LV_ALIGN_LEFT_MID, 68, 0);

    s_edited_badge = lv_label_create(bar);
    lv_label_set_text(s_edited_badge, "EDITED");
    lv_obj_set_style_bg_color(s_edited_badge, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_edited_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_edited_badge, lv_color_hex(0x101013), 0);
    lv_obj_set_style_pad_hor(s_edited_badge, 8, 0);
    lv_obj_set_style_pad_ver(s_edited_badge, 3, 0);
    lv_obj_set_style_radius(s_edited_badge, 5, 0);
    lv_obj_align(s_edited_badge, LV_ALIGN_LEFT_MID, 168, 0);
    lv_obj_add_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *save_btn = lv_button_create(bar);
    lv_obj_set_size(save_btn, 100, 38);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, 9, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "SAVE");
    lv_obj_set_style_text_color(save_label, lv_color_hex(0x161409), 0);
    lv_obj_center(save_label);
}

static void build_chain_rail(lv_obj_t *screen) {
    s_chain_rail = lv_obj_create(screen);
    lv_obj_remove_style_all(s_chain_rail);
    lv_obj_set_size(s_chain_rail, LV_PCT(100), 112);
    lv_obj_align(s_chain_rail, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(s_chain_rail, UI_COLOR_RAIL, 0);
    lv_obj_set_style_bg_opa(s_chain_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_chain_rail, 28, 0);
    lv_obj_set_flex_flow(s_chain_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_chain_rail, 20, 0);
    lv_obj_set_flex_align(s_chain_rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_chain_rail, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < UI_EFFECT_COUNT; i++) {
        uint8_t fx = s_chain_order[i];
        lv_obj_t *chip = ui_effect_chip_create(s_chain_rail, fx);
        s_chips[fx] = chip;
    }
    refresh_all_chip_styles();
}

static void build_fx_panel(lv_obj_t *screen) {
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), 346);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(panel, 32, 0);
    lv_obj_set_style_pad_top(panel, 22, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_fx_swatch = lv_obj_create(panel);
    lv_obj_remove_style_all(s_fx_swatch);
    lv_obj_set_size(s_fx_swatch, 10, 10);
    lv_obj_set_style_radius(s_fx_swatch, 3, 0);
    lv_obj_set_style_bg_opa(s_fx_swatch, LV_OPA_COVER, 0);
    lv_obj_align(s_fx_swatch, LV_ALIGN_TOP_LEFT, 0, 4);

    s_fx_panel_title = lv_label_create(panel);
    lv_obj_align(s_fx_panel_title, LV_ALIGN_TOP_LEFT, 24, 0);

    s_fx_model_dropdown = lv_dropdown_create(panel);
    lv_obj_set_size(s_fx_model_dropdown, 220, 36);
    lv_obj_align(s_fx_model_dropdown, LV_ALIGN_TOP_LEFT, 120, -6);
    lv_obj_set_style_bg_color(s_fx_model_dropdown, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_fx_model_dropdown, 1, 0);
    lv_obj_set_style_border_color(s_fx_model_dropdown, UI_COLOR_BORDER, 0);

    s_knob_row = lv_obj_create(panel);
    lv_obj_remove_style_all(s_knob_row);
    lv_obj_set_size(s_knob_row, LV_PCT(100), 220);
    lv_obj_align(s_knob_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(s_knob_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_knob_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_knob_row, LV_OBJ_FLAG_SCROLLABLE);

    rebuild_panel();
}

static void build_bottom_bar(lv_obj_t *screen) {
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 82);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 28, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *presets_btn = lv_button_create(bar);
    lv_obj_set_size(presets_btn, 110, 40);
    lv_obj_align(presets_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(presets_btn, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(presets_btn, 1, 0);
    lv_obj_set_style_border_color(presets_btn, UI_COLOR_BORDER, 0);
    lv_obj_add_event_cb(presets_btn, presets_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *presets_label = lv_label_create(presets_btn);
    lv_label_set_text(presets_label, "Menu");
    lv_obj_center(presets_label);

    lv_obj_t *settings_btn = lv_button_create(bar);
    lv_obj_set_size(settings_btn, 110, 40);
    lv_obj_align(settings_btn, LV_ALIGN_LEFT_MID, 122, 0);
    lv_obj_set_style_bg_color(settings_btn, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(settings_btn, 1, 0);
    lv_obj_set_style_border_color(settings_btn, UI_COLOR_BORDER, 0);
    lv_obj_add_event_cb(settings_btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *settings_label = lv_label_create(settings_btn);
    lv_label_set_text(settings_label, "Settings");
    lv_obj_center(settings_label);

    lv_obj_t *master_caption = lv_label_create(bar);
    lv_label_set_text(master_caption, "MASTER");
    lv_obj_set_style_text_color(master_caption, lv_color_hex(0x78746c), 0);
    lv_obj_align(master_caption, LV_ALIGN_CENTER, -220, 0);

    s_master_slider = lv_slider_create(bar);
    lv_slider_set_range(s_master_slider, 0, 100);
    lv_slider_set_value(s_master_slider, s_master_volume, LV_ANIM_OFF);
    lv_obj_set_size(s_master_slider, 360, 10);
    lv_obj_align(s_master_slider, LV_ALIGN_CENTER, 10, 0);
    lv_obj_set_style_bg_color(s_master_slider, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_master_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_master_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_master_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_master_slider, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_master_slider, master_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_mute_btn = lv_button_create(bar);
    lv_obj_set_size(s_mute_btn, 90, 40);
    lv_obj_align(s_mute_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_mute_btn, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_mute_btn, 1, 0);
    lv_obj_set_style_border_color(s_mute_btn, UI_COLOR_BORDER, 0);
    lv_obj_add_event_cb(s_mute_btn, mute_btn_cb, LV_EVENT_CLICKED, NULL);
    s_mute_label = lv_label_create(s_mute_btn);
    lv_label_set_text(s_mute_label, "MUTE");
    lv_obj_center(s_mute_label);
}

static void build_settings_overlay(lv_obj_t *screen) {
    s_settings_backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(s_settings_backdrop);
    lv_obj_set_size(s_settings_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_settings_backdrop, lv_color_hex(0x06060a), 0);
    lv_obj_set_style_bg_opa(s_settings_backdrop, 160, 0);
    lv_obj_add_flag(s_settings_backdrop, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_settings_backdrop, settings_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_settings_backdrop);
    lv_obj_set_size(card, 400, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 20, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // see ui_vertical_slider.c: absorbs
                                                   // taps so they don't fall through
                                                   // to the backdrop's close handler

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *bl_caption = lv_label_create(card);
    lv_label_set_text(bl_caption, "BACKLIGHT");
    lv_obj_set_style_text_color(bl_caption, lv_color_hex(0x78746c), 0);

    s_backlight_slider = lv_slider_create(card);
    lv_slider_set_range(s_backlight_slider, 10, 100);
    lv_slider_set_value(s_backlight_slider, s_backlight, LV_ANIM_OFF);
    lv_obj_set_size(s_backlight_slider, LV_PCT(100), 10);
    lv_obj_set_style_bg_color(s_backlight_slider, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_backlight_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_backlight_slider, backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi_row = lv_obj_create(card);
    lv_obj_remove_style_all(wifi_row);
    lv_obj_set_size(wifi_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *wifi_label = lv_label_create(wifi_row);
    lv_label_set_text(wifi_label, "Wi-Fi");
    s_wifi_switch = make_pill_switch(wifi_row, s_wifi_on, wifi_switch_cb, &s_wifi_knob);

    lv_obj_t *ble_row = lv_obj_create(card);
    lv_obj_remove_style_all(ble_row);
    lv_obj_set_size(ble_row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(ble_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ble_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ble_label = lv_label_create(ble_row);
    lv_label_set_text(ble_label, "Bluetooth");
    s_ble_switch = make_pill_switch(ble_row, s_ble_on, ble_switch_cb, &s_ble_knob);
}

static void build_side_drawer(lv_obj_t *screen) {
    s_drawer_backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(s_drawer_backdrop);
    lv_obj_set_size(s_drawer_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_drawer_backdrop, lv_color_hex(0x06060a), 0);
    lv_obj_set_style_bg_opa(s_drawer_backdrop, 160, 0);
    lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_drawer_backdrop, drawer_backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *drawer = lv_obj_create(s_drawer_backdrop);
    lv_obj_set_size(drawer, 300, LV_PCT(100));
    lv_obj_align(drawer, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(drawer, lv_color_hex(0x18181d), 0);
    lv_obj_set_style_radius(drawer, 0, 0);
    lv_obj_set_style_border_side(drawer, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(drawer, UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(drawer, 22, 0);
    lv_obj_set_style_pad_row(drawer, 10, 0);
    lv_obj_set_flex_flow(drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(drawer, LV_OBJ_FLAG_CLICKABLE); // absorbs taps so they don't close the drawer

    lv_obj_t *title = lv_label_create(drawer);
    lv_label_set_text(title, "Menu");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);

    lv_obj_t *spacer = lv_obj_create(drawer);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *dev = lv_label_create(drawer);
    lv_label_set_text(dev, "DEVELOPER");
    lv_obj_set_style_text_font(dev, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(dev, lv_color_hex(0x5a5852), 0);

    lv_obj_t *test_btn = lv_button_create(drawer);
    lv_obj_set_size(test_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x211c1c), 0);
    lv_obj_set_style_border_color(test_btn, lv_color_hex(0x3a2a2a), 0);
    lv_obj_set_style_border_width(test_btn, 1, 0);
    lv_obj_set_style_shadow_width(test_btn, 0, 0);
    lv_obj_add_event_cb(test_btn, test_mode_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *test_l = lv_label_create(test_btn);
    lv_label_set_text(test_l, "Test Mode");
    lv_obj_set_style_text_color(test_l, UI_COLOR_MUTE, 0);
    lv_obj_align(test_l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *test_sub = lv_label_create(test_btn);
    lv_label_set_text(test_sub, "ESP32 + SEED3");
    lv_obj_set_style_text_font(test_sub, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(test_sub, UI_COLOR_TEXT_DIM, 0);
    lv_obj_align(test_sub, LV_ALIGN_RIGHT_MID, -4, 0);
}

void ui_main_init(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(screen);
    build_chain_rail(screen);
    build_fx_panel(screen);
    build_bottom_bar(screen);
    ui_vertical_slider_init(screen); // must come after the panel exists, before load
    build_settings_overlay(screen);
    build_side_drawer(screen);
    ui_test_mode_init(screen);

    lv_screen_load(screen);
}
