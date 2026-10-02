#include "ui_main.h"
#include "ui_theme.h"
#include "ui_effects_data.h"
#include "ui_effect_chip.h"
#include "ui_knob.h"
#include "ui_ir_curve.h"
#include "ui_vertical_slider.h"
#include "ui_test_mode.h"
#include "board_init.h"
#include "seed_link.h"
#include "rig_state.h"
#include "amp_models.h"
#include "nam_store.h"
#include "seed_link_proto.h"
#include "esp_log.h"
#include "nvs.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui";

// ---------------------------------------------------------------------------------------------
// App state. Mirrors the preview's App component state (preview/index.html). The audio state
// (chain, on/bypass, models, knobs, master, mute) lives in rig_state: read it through rig(),
// change it only through the rig_set_*() calls, which also tell the Seed link.

static uint8_t s_selected_fx = FX_AMP;
static bool s_dirty = false;
static bool s_wifi_on = true;   // TODO: drive the real radios; these are UI settings only so far
static bool s_ble_on = true;
static char s_preset_name[32] = "Preset 1";

// ---------------------------------------------------------------------------------------------
// Widgets

static lv_obj_t *s_chips[UI_EFFECT_COUNT];   // indexed by fx, NULL when not in the chain
static lv_obj_t *s_rail;                     // scrollable chain (chips + "+" button)
static lv_obj_t *s_fx_swatch, *s_fx_title, *s_fx_subtitle;
static lv_obj_t *s_model_row, *s_amp_status;
static lv_obj_t *s_knob_row;
static lv_obj_t *s_curve_col, *s_curve_caption;
static lv_obj_t *s_preset_label, *s_edited_badge;
static lv_obj_t *s_wifi_dot, *s_ble_dot;
static lv_obj_t *s_meter_in, *s_meter_out, *s_meter_mock;
static lv_obj_t *s_master_slider, *s_master_value;
static lv_obj_t *s_mute_btn, *s_mute_label;
static lv_obj_t *s_settings_backdrop, *s_backlight_slider, *s_backlight_value;
static lv_obj_t *s_wifi_switch, *s_wifi_knob, *s_ble_switch, *s_ble_knob;
static lv_obj_t *s_drawer_backdrop;
static lv_obj_t *s_preset_names[UI_PRESET_COUNT];
static lv_obj_t *s_catalog_backdrop;
static lv_obj_t *s_catalog_pills[UI_CHAIN_MAX];

static void rebuild_rail(void);
static void rebuild_panel(void);
static void update_cab_curve(void);
static void apply_external_changes(void);

// ---------------------------------------------------------------------------------------------
// Persistence: SAVE writes the whole rig to NVS; it's restored at boot. Settings (backlight,
// radios) are stored separately whenever the Settings card closes.

#define NVS_NS        "ui"
#define NVS_KEY_RIG   "rig"
#define NVS_KEY_SET   "settings"
#define RIG_MAGIC_V1  0x46585331u  // "FXS1": before SD amp profiles
#define RIG_MAGIC     0x46585332u  // "FXS2": adds the SD amp profile reference

// Version 1 layout, kept only to migrate rigs saved by older firmware.
typedef struct {
    uint32_t magic;
    uint8_t chain_len;
    uint8_t chain[UI_CHAIN_MAX];
    uint8_t on[UI_EFFECT_COUNT];
    uint8_t model[UI_EFFECT_COUNT];
    int8_t knobs[UI_EFFECT_COUNT][UI_MAX_KNOBS];
    uint8_t master_volume;
    char preset_name[32];
} saved_rig_v1_t;

typedef struct {
    uint32_t magic;
    uint8_t chain_len;
    uint8_t chain[UI_CHAIN_MAX];
    uint8_t on[UI_EFFECT_COUNT];
    uint8_t model[UI_EFFECT_COUNT];        // for the AMP: built-in index, used when amp_sd_hash is 0
    int8_t knobs[UI_EFFECT_COUNT][UI_MAX_KNOBS];
    uint8_t master_volume;
    char preset_name[32];
    uint32_t amp_sd_hash;                  // SD amp profile (CRC32 of its weights), 0 = built-in
    char amp_sd_name[32];                  // its name, to say which profile is missing if the card lacks it
} saved_rig_t;

typedef struct {
    uint8_t backlight_pct;
    uint8_t wifi_on, ble_on;
} saved_settings_t;

static void save_rig(void) {
    rig_full_t cur;
    rig_get_full(&cur);
    saved_rig_t r = { .magic = RIG_MAGIC, .chain_len = cur.r.chain_len, .master_volume = (uint8_t)cur.r.master_volume };
    memcpy(r.chain, cur.r.chain, sizeof(r.chain));
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        r.on[fx] = cur.r.on[fx];
        r.model[fx] = cur.r.model[fx];
        for (int k = 0; k < UI_MAX_KNOBS; k++) r.knobs[fx][k] = (int8_t)cur.knobs[fx][k];
    }
    strlcpy(r.preset_name, s_preset_name, sizeof(r.preset_name));
    r.amp_sd_hash = cur.r.amp_sd_hash;
    amp_model_t am;
    if (r.amp_sd_hash && amp_models_get(amp_models_find_sd(r.amp_sd_hash), &am)) {
        strlcpy(r.amp_sd_name, am.name, sizeof(r.amp_sd_name));
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_RIG, &r, sizeof(r));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "saving rig failed: %s", esp_err_to_name(err));
}

static bool rig_is_valid(const saved_rig_t *r) {
    if (r->magic != RIG_MAGIC || r->chain_len == 0 || r->chain_len > UI_CHAIN_MAX) return false;
    uint32_t seen = 0;
    for (int i = 0; i < r->chain_len; i++) {
        if (r->chain[i] >= FX_CAB || (seen & (1u << r->chain[i]))) return false;
        seen |= 1u << r->chain[i];
    }
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        const int models = fx == FX_AMP ? amp_models_builtin_count() : g_effects[fx].model_count;
        if (models ? r->model[fx] >= models : r->model[fx] != 0) return false;
        for (int k = 0; k < UI_MAX_KNOBS; k++) if (r->knobs[fx][k] < 0 || r->knobs[fx][k] > 100) return false;
    }
    return r->master_volume <= 100;
}

static void load_saved_state(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // nothing saved yet

    saved_rig_t r = { 0 };
    size_t len = sizeof(r);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_RIG, &r, &len);
    if (err == ESP_OK && len == sizeof(saved_rig_v1_t) && r.magic == RIG_MAGIC_V1) {
        // Older firmware listed four amps (Twin, AC30, Plexi, Mesa); the Seed has three
        // (Twin65, AC30, JCM800 G5). Plexi and Mesa both become the JCM800 G5.
        saved_rig_v1_t v1;
        memcpy(&v1, &r, sizeof(v1));
        memset(&r, 0, sizeof(r));
        memcpy(&r, &v1, sizeof(v1)); // same leading layout
        r.magic = RIG_MAGIC;
        if (r.model[FX_AMP] > 2) r.model[FX_AMP] = 2;
        len = sizeof(r);
        ESP_LOGI(TAG, "migrated saved rig from v1");
    }
    if (err == ESP_OK && len == sizeof(r) && rig_is_valid(&r)) {
        rig_full_t full = { .r = { .chain_len = r.chain_len, .master_volume = r.master_volume,
                                   .amp_sd_hash = r.amp_sd_hash } };
        memcpy(full.r.chain, r.chain, sizeof(full.r.chain));
        for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
            full.r.on[fx] = r.on[fx] || fx == FX_CAB;
            full.r.model[fx] = r.model[fx];
            for (int k = 0; k < UI_MAX_KNOBS; k++) full.knobs[fx][k] = r.knobs[fx][k];
        }
        rig_replace(&full, RIG_SRC_BOOT);
        r.preset_name[sizeof(r.preset_name) - 1] = '\0';
        strlcpy(s_preset_name, r.preset_name, sizeof(s_preset_name));
        s_selected_fx = rig()->chain[0];
        ESP_LOGI(TAG, "restored saved rig \"%s\"", s_preset_name);
    }

    saved_settings_t st;
    len = sizeof(st);
    if (nvs_get_blob(h, NVS_KEY_SET, &st, &len) == ESP_OK && len == sizeof(st)) {
        board_set_backlight_pct(st.backlight_pct);
        s_wifi_on = st.wifi_on;
        s_ble_on = st.ble_on;
    }
    nvs_close(h);
}

static void save_settings(void) {
    saved_settings_t st = { board_get_backlight_pct(), s_wifi_on, s_ble_on };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY_SET, &st, sizeof(st)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------------------------
// Small widget helpers

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *make_row(lv_obj_t *parent, int32_t gap) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static lv_obj_t *make_ghost_btn(lv_obj_t *parent, const char *text, int32_t w, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_bg_color(b, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

// Full-screen dimmed backdrop; tapping it (outside the card on top) calls close_cb.
static lv_obj_t *make_backdrop(lv_obj_t *screen, lv_event_cb_t close_cb) {
    lv_obj_t *b = lv_obj_create(screen);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(b, UI_COLOR_BACKDROP, 0);
    lv_obj_set_style_bg_opa(b, 160, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, close_cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// Popup card. Clickable so taps on its empty areas are absorbed instead of falling through to
// the backdrop's close handler (see ui_vertical_slider.c for the full explanation).
static lv_obj_t *make_card(lv_obj_t *backdrop, int32_t w, int32_t h) {
    lv_obj_t *c = lv_obj_create(backdrop);
    lv_obj_set_size(c, w, h);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_color(c, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

// Floating close button for a card's top-right corner.
static void make_close_btn(lv_obj_t *card, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(card);
    lv_obj_set_size(b, 40, 40);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 12, -12);
    lv_obj_set_style_bg_opa(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = make_label(b, LV_SYMBOL_CLOSE, NULL, UI_COLOR_TEXT_MUTED);
    lv_obj_center(l);
}

static void set_switch(lv_obj_t *sw, lv_obj_t *knob, bool on) {
    lv_obj_set_style_bg_color(sw, on ? UI_COLOR_ACCENT : UI_COLOR_TRACK, 0);
    lv_obj_align(knob, on ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, on ? -3 : 3, 3);
}

static lv_obj_t *make_pill_switch(lv_obj_t *parent, bool initial, lv_event_cb_t cb, lv_obj_t **out_knob) {
    lv_obj_t *sw = lv_obj_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 46, 26);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sw, 10);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, 20, 20);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    set_switch(sw, knob, initial);

    *out_knob = knob;
    return sw;
}

static void style_slider(lv_obj_t *s) {
    lv_obj_set_style_bg_color(s, UI_COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 4, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, UI_COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
}

// ---------------------------------------------------------------------------------------------
// State changes

static void refresh_all_chip_styles(void) {
    for (uint8_t fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        if (s_chips[fx]) ui_effect_chip_refresh(s_chips[fx], rig()->on[fx], s_selected_fx == fx);
    }
}

static void refresh_preset_names(void) {
    for (int i = 0; i < UI_PRESET_COUNT; i++) {
        lv_obj_set_style_text_color(s_preset_names[i],
            strcmp(s_preset_name, g_presets[i].name) == 0 ? UI_COLOR_ACCENT : UI_COLOR_TEXT, 0);
    }
}

static void set_dirty(bool dirty) {
    s_dirty = dirty;
    if (dirty) lv_obj_clear_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);
}

void ui_main_mark_dirty(void) {
    if (!s_dirty) set_dirty(true);
}

static void select_fx(uint8_t fx) {
    s_selected_fx = fx;
    rebuild_panel();
    refresh_all_chip_styles();
    if (s_chips[fx]) lv_obj_scroll_to_view(s_chips[fx], LV_ANIM_ON);
}

void ui_main_on_chip_tapped(uint8_t fx_index) {
    if (s_selected_fx != fx_index) {
        select_fx(fx_index);
        return;
    }
    // Already the open chip: second tap bypasses, third tap re-enables, and so on.
    rig_set_fx_on(fx_index, !rig()->on[fx_index], RIG_SRC_UI);
    refresh_all_chip_styles();
    ui_main_mark_dirty();
}

void ui_main_on_chip_reordered(lv_obj_t *rail) {
    uint8_t chain[UI_CHAIN_MAX];
    uint8_t n = 0;
    uint32_t cnt = lv_obj_get_child_cnt(rail);
    for (uint32_t i = 0; i < cnt && n < UI_CHAIN_MAX; i++) {
        lv_obj_t *c = lv_obj_get_child(rail, i);
        if (lv_obj_has_flag(c, UI_CHIP_FLAG)) chain[n++] = (uint8_t)(uintptr_t)lv_obj_get_user_data(c);
    }
    // A drop at the chip's own slot is not a change.
    if (rig_set_chain(chain, n, RIG_SRC_UI)) ui_main_mark_dirty();
}

void ui_main_on_knob_tapped(uint8_t fx_index, uint8_t knob_index) {
    ui_vertical_slider_show(fx_index, knob_index);
}

void ui_main_on_knob_changed(uint8_t fx_index, uint8_t knob_index) {
    if (fx_index == s_selected_fx && knob_index < lv_obj_get_child_cnt(s_knob_row)) {
        ui_knob_set_value(lv_obj_get_child(s_knob_row, knob_index), g_knob_values[fx_index][knob_index]);
        if (fx_index == FX_CAB) update_cab_curve();
    }
    ui_main_mark_dirty();
}

// Loads a preset as one change: the Seed gets exactly one snapshot for it.
static void apply_preset(const ui_preset_t *p) {
    rig_full_t full = { .r = { .chain_len = p->chain_len,
                               .master_volume = rig()->master_volume, .muted = rig()->muted } };
    memcpy(full.r.chain, p->chain, sizeof(full.r.chain));
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        full.r.on[fx] = (p->on_mask & (1u << fx)) || fx == FX_CAB;
        full.r.model[fx] = p->models[fx];
        for (int k = 0; k < UI_MAX_KNOBS; k++) full.knobs[fx][k] = g_default_knob_values[fx][k];
    }
    for (int i = 0; i < p->knob_count; i++) {
        for (int k = 0; k < UI_MAX_KNOBS; k++) full.knobs[p->knobs[i].fx][k] = p->knobs[i].values[k];
    }
    rig_replace(&full, RIG_SRC_PRESET);
    strlcpy(s_preset_name, p->name, sizeof(s_preset_name));
    lv_label_set_text(s_preset_label, s_preset_name);
    s_selected_fx = rig()->chain[0];
    set_dirty(false);
    rebuild_rail();
    rebuild_panel();
    refresh_preset_names();
}

// ---------------------------------------------------------------------------------------------
// Top bar: preset name + EDITED badge on the left; radio status and SAVE on the right.

static void save_btn_cb(lv_event_t *e) {
    (void)e;
    save_rig();
    set_dirty(false);
}

static lv_obj_t *make_status(lv_obj_t *parent, const char *text, lv_obj_t **dot_out) {
    lv_obj_t *r = make_row(parent, 6);
    lv_obj_t *dot = lv_obj_create(r);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    make_label(r, text, &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    *dot_out = dot;
    return r;
}

static void refresh_status_dots(void) {
    lv_obj_set_style_bg_color(s_wifi_dot, s_wifi_on ? UI_COLOR_OK : UI_COLOR_OFF, 0);
    lv_obj_set_style_bg_color(s_ble_dot, s_ble_on ? UI_COLOR_OK : UI_COLOR_OFF, 0);
}

static void build_top_bar(lv_obj_t *screen) {
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 60);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_pad_hor(bar, 28, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = make_row(bar, 12);
    make_label(left, "PRESET", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    s_preset_label = make_label(left, s_preset_name, &lv_font_montserrat_16, UI_COLOR_TEXT);
    s_edited_badge = make_label(left, "EDITED", &lv_font_montserrat_12, UI_COLOR_ON_ACCENT);
    lv_obj_set_style_bg_color(s_edited_badge, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_edited_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_edited_badge, 8, 0);
    lv_obj_set_style_pad_ver(s_edited_badge, 3, 0);
    lv_obj_set_style_radius(s_edited_badge, 5, 0);
    lv_obj_add_flag(s_edited_badge, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *right = make_row(bar, 20);
    make_status(right, "Wi-Fi", &s_wifi_dot);
    make_status(right, "BLE", &s_ble_dot);
    refresh_status_dots();

    lv_obj_t *save_btn = lv_button_create(right);
    lv_obj_set_size(save_btn, 100, 38);
    lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(save_btn, 9, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = make_label(save_btn, "SAVE", NULL, UI_COLOR_ON_ACCENT);
    lv_obj_center(save_label);
}

// ---------------------------------------------------------------------------------------------
// Chain rail: scrollable effects + "+" (add/remove catalog), then the pinned CAB.

static void catalog_open_cb(lv_event_t *e);

static void rebuild_rail(void) {
    for (int fx = 0; fx < FX_CAB; fx++) s_chips[fx] = NULL;
    lv_obj_clean(s_rail);
    const rig_t *r = rig();
    for (uint8_t i = 0; i < r->chain_len; i++) s_chips[r->chain[i]] = ui_effect_chip_create(s_rail, r->chain[i]);

    lv_obj_t *plus = lv_button_create(s_rail);
    lv_obj_set_size(plus, 56, UI_CHIP_H);
    lv_obj_set_style_bg_opa(plus, 0, 0);
    lv_obj_set_style_border_width(plus, 2, 0);
    lv_obj_set_style_border_color(plus, UI_COLOR_BORDER, 0);
    lv_obj_set_style_radius(plus, 14, 0);
    lv_obj_add_event_cb(plus, catalog_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = make_label(plus, LV_SYMBOL_PLUS, &lv_font_montserrat_16, UI_COLOR_TEXT_MUTED);
    lv_obj_center(pl);

    refresh_all_chip_styles();
}

static void build_chain_rail(lv_obj_t *screen) {
    lv_obj_t *band = lv_obj_create(screen);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LV_PCT(100), 112);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(band, UI_COLOR_RAIL, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(band, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(band, 1, 0);
    lv_obj_set_style_border_color(band, UI_COLOR_DIVIDER, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    s_rail = lv_obj_create(band);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_height(s_rail, LV_PCT(100));
    lv_obj_set_flex_grow(s_rail, 1);
    lv_obj_set_style_pad_left(s_rail, 28, 0);
    lv_obj_set_style_pad_right(s_rail, 20, 0);
    lv_obj_set_style_pad_column(s_rail, 14, 0);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_rail, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_rail, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_rail, UI_COLOR_BORDER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_rail, 4, LV_PART_SCROLLBAR);

    lv_obj_t *pinned = make_row(band, 14);
    lv_obj_set_height(pinned, LV_PCT(100));
    lv_obj_set_style_pad_left(pinned, 18, 0);
    lv_obj_set_style_pad_right(pinned, 28, 0);
    lv_obj_set_style_border_side(pinned, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(pinned, 1, 0);
    lv_obj_set_style_border_color(pinned, UI_COLOR_DIVIDER, 0);
    make_label(pinned, LV_SYMBOL_RIGHT, &lv_font_montserrat_12, UI_COLOR_GLYPH);
    s_chips[FX_CAB] = ui_effect_chip_create(pinned, FX_CAB);

    rebuild_rail();
}

// ---------------------------------------------------------------------------------------------
// Effect panel: title, model cards, knobs (+ CAB curve), and the IN/OUT meters on the right.

static void update_cab_curve(void) {
    const ui_effect_def_t *def = &g_effects[FX_CAB];
    char cap[64];
    const uint8_t model = rig()->model[FX_CAB];
    int n = snprintf(cap, sizeof(cap), "FREQUENCY RESPONSE - %s", def->models[model].name);
    for (int i = 0; i < n && cap[i]; i++) cap[i] = (char)toupper((unsigned char)cap[i]);
    lv_label_set_text(s_curve_caption, cap);
    ui_ir_curve_update(model, g_knob_values[FX_CAB][0], g_knob_values[FX_CAB][1], def->color);
}

// Model cards. The AMP's list is the Seed's built-in amps plus SD amp profiles (amp_models.h);
// every other effect's list is fixed in ui_effects_data.c.
static uint32_t s_amp_cards_version;   // amp_models_version() the AMP cards were built from
#define LIBRARY_CARD 0xFFFF                 // model card index of the "SD Library" card
static lv_obj_t *s_lib_backdrop;           // SD Library popup, built in build_sd_library()
static uint32_t s_lib_version;
static void lib_fill(void);

static int card_count(uint8_t fx) {
    return fx == FX_AMP ? amp_models_count() : g_effects[fx].model_count;
}

// The card that shows as selected, or -1 (an SD profile that isn't on the card).
static int selected_card(uint8_t fx) {
    const rig_t *r = rig();
    if (fx == FX_AMP && r->amp_sd_hash) return amp_models_find_sd(r->amp_sd_hash);
    return r->model[fx];
}

static void restyle_model_cards(uint8_t fx) {
    const lv_color_t color = g_effects[fx].color;
    const int sel = selected_card(fx);
    const uint32_t n = lv_obj_get_child_cnt(s_model_row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *card = lv_obj_get_child(s_model_row, i);
        const int index = (int)(uintptr_t)lv_obj_get_user_data(card);
        lv_obj_set_style_border_color(card, index == sel ? color : UI_COLOR_TRACK, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(card, 0), index == sel ? color : UI_COLOR_TEXT, 0);
    }
}

static void lib_open(void);

static void model_card_cb(lv_event_t *e) {
    const int m = (int)(uintptr_t)lv_event_get_user_data(e);
    const uint8_t fx = s_selected_fx;
    if (m == LIBRARY_CARD) {
        lib_open();
        return;
    }
    const ui_effect_def_t *def = &g_effects[fx];
    if (fx == FX_AMP) {
        amp_model_t am;
        if (!amp_models_get(m, &am) || am.rejected) return; // a file that can't run: its card says why
        if (am.builtin) {
            // A built-in also loads its suggested knob positions, as the other effects' models do.
            const int8_t *values = m < def->model_count ? def->models[m].values : NULL;
            rig_set_model(fx, (uint8_t)m, values, RIG_SRC_UI);
        } else {
            rig_set_amp_sd(am.sd_hash, RIG_SRC_UI); // the link uploads it if the Seed lacks it
        }
    } else {
        // The model and the knob values it brings go to the Seed as one change.
        rig_set_model(fx, (uint8_t)m, def->models[m].values, RIG_SRC_UI);
    }

    // Restyle in place rather than rebuild_panel(): that would delete the card whose CLICKED
    // event is still being dispatched.
    restyle_model_cards(fx);
    for (uint8_t k = 0; k < def->knob_count; k++) ui_knob_set_value(lv_obj_get_child(s_knob_row, k), g_knob_values[fx][k]);
    if (fx == FX_CAB) update_cab_curve();
    ui_main_mark_dirty();
}

#define MODEL_CARD_H 72 // name + two description lines; every card in the row is this tall

static void add_model_card(int index, const char *title, const char *desc, bool sel, lv_color_t color, bool disabled) {
    lv_obj_t *card = lv_obj_create(s_model_row);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 196, MODEL_CARD_H);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, sel ? color : UI_COLOR_TRACK, 0);
    lv_obj_set_style_radius(card, 11, 0);
    lv_obj_set_style_pad_hor(card, 13, 0);
    lv_obj_set_style_pad_ver(card, 10, 0);
    lv_obj_set_style_pad_row(card, 3, 0);
    lv_obj_set_style_bg_color(card, UI_COLOR_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, model_card_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_set_user_data(card, (void *)(uintptr_t)index);

    lv_obj_t *name = make_label(card, title, &lv_font_montserrat_14, sel ? color : disabled ? UI_COLOR_TEXT_MUTED : UI_COLOR_TEXT);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *d = make_label(card, desc, &lv_font_montserrat_12, disabled ? UI_COLOR_WARN : UI_COLOR_TEXT_MUTED);
    lv_obj_set_size(d, LV_PCT(100), 2 * lv_font_get_line_height(&lv_font_montserrat_12));
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT); // at most two lines, then "..."
}

static void build_model_cards(const ui_effect_def_t *def) {
    lv_obj_clean(s_model_row);
    const uint8_t fx = s_selected_fx;
    if (!def->models) {
        lv_obj_add_flag(s_model_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_model_row, LV_OBJ_FLAG_HIDDEN);
    const int sel = selected_card(fx);
    const int n = card_count(fx);
    if (fx != FX_AMP) {
        for (int m = 0; m < n; m++) add_model_card(m, def->models[m].name, def->models[m].desc, m == sel, def->color, false);
        return;
    }
    // AMP: the Seed's built-in amps, the selected SD profile (if any), then the SD Library card,
    // which lists everything on the card at full width.
    const int builtins = amp_models_builtin_count();
    amp_model_t am;
    for (int m = 0; m < builtins && amp_models_get(m, &am); m++) add_model_card(m, am.name, am.desc, m == sel, def->color, false);
    if (sel >= builtins && amp_models_get(sel, &am)) add_model_card(sel, am.name, "From the SD card", true, def->color, false);
    nam_store_status_t st;
    nam_store_status(&st);
    char desc[72];
    if (st.scanning) snprintf(desc, sizeof(desc), "Reading the SD card...");
    else if (!st.card_ok) snprintf(desc, sizeof(desc), "No usable SD card. Tap for details.");
    else snprintf(desc, sizeof(desc), "%d profile%s%s. Tap to browse.", st.accepted, st.accepted == 1 ? "" : "s",
                  st.rejected ? ", some can't be used" : "");
    add_model_card(LIBRARY_CARD, "SD Library  " LV_SYMBOL_RIGHT, desc, false, def->color, false);
    s_amp_cards_version = amp_models_version();
}

// One line under the AMP cards saying what the Seed is actually running. Only real link data:
// no link means "not connected", never a guess.
static void amp_status_text(char *buf, size_t len) {
    seed_link_stats_t s;
    seed_link_get_stats(&s);
    const rig_t *r = rig();
    const char *mock = s.peer_mock ? " (MOCK Seed)" : "";
    if (!s.connected) {
        snprintf(buf, len, "Seed not connected: the amp is applied when it connects.");
        return;
    }
    if (r->amp_sd_hash && amp_models_find_sd(r->amp_sd_hash) < 0) {
        snprintf(buf, len, "This rig's SD amp profile (%08lx) is not on the card.", (unsigned long)r->amp_sd_hash);
        return;
    }
    if (r->amp_sd_hash && s.upload_hash == r->amp_sd_hash && s.upload_state == SEED_UPLOAD_RUNNING) {
        snprintf(buf, len, "Uploading to the Seed: %u%%%s", s.upload_pct, mock);
        return;
    }
    if (r->amp_sd_hash && s.upload_hash == r->amp_sd_hash && s.upload_state == SEED_UPLOAD_FAILED) {
        snprintf(buf, len, "Upload failed: %s. Retrying.", s.upload_note);
        return;
    }
    if (s.status_flags & SLP_STATUS_MODEL_FAILED) {
        snprintf(buf, len, "The Seed failed to load this model%s.", mock);
        return;
    }
    if (!r->on[FX_AMP]) {
        snprintf(buf, len, "Amp bypassed: no model in the signal path%s.", mock);
        return;
    }
    amp_model_t am;
    const bool running = r->amp_sd_hash
        ? (s.active_kind == SLP_MODEL_UPLOADED && s.active_hash == r->amp_sd_hash)
        : (amp_models_get(r->model[FX_AMP], &am) && s.active_kind == SLP_MODEL_BUILTIN && s.active_builtin == am.builtin_id);
    snprintf(buf, len, running ? "Running on the Seed%s." : "Switching model on the Seed%s...", mock);
}

static void refresh_amp_status(void) {
    if (s_selected_fx != FX_AMP) return;
    char text[200];
    amp_status_text(text, 100);
    nam_store_status_t sd;
    nam_store_status(&sd);
    const size_t n = strlen(text);
    snprintf(text + n, sizeof(text) - n, "\n%s", sd.status);
    if (strcmp(lv_label_get_text(s_amp_status), text) != 0) lv_label_set_text(s_amp_status, text);
}

static void rebuild_panel(void) {
    const ui_effect_def_t *def = &g_effects[s_selected_fx];

    lv_obj_set_style_bg_color(s_fx_swatch, def->color, 0);
    lv_label_set_text(s_fx_title, def->label);
    if (def->subtitle) {
        lv_label_set_text(s_fx_subtitle, def->subtitle);
        lv_obj_clear_flag(s_fx_subtitle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_fx_subtitle, LV_OBJ_FLAG_HIDDEN);
    }

    build_model_cards(def);
    if (s_selected_fx == FX_AMP) {
        lv_obj_clear_flag(s_amp_status, LV_OBJ_FLAG_HIDDEN);
        refresh_amp_status();
    } else {
        lv_obj_add_flag(s_amp_status, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clean(s_knob_row);
    const bool is_cab = s_selected_fx == FX_CAB;
    const int32_t d = is_cab ? 104 : ui_knob_diameter_for(def->knob_count);
    for (uint8_t k = 0; k < def->knob_count; k++) ui_knob_create(s_knob_row, s_selected_fx, k, d);

    if (is_cab) {
        update_cab_curve();
        lv_obj_clear_flag(s_curve_col, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_curve_col, LV_OBJ_FLAG_HIDDEN);
    }
}

// Level meters show SEED3 peaks only; with no link (or no recent METERS) they stay empty, and
// levels simulated by the mock Seed are tagged MOCK. Never invented levels.
#define METER_H 180
static int32_t meter_px(float dbfs) {
    if (dbfs <= -60.0f) return 0;
    if (dbfs >= 0.0f) return METER_H;
    return (int32_t)((dbfs + 60.0f) / 60.0f * METER_H);
}

static void meter_timer_cb(lv_timer_t *t) {
    (void)t;
    apply_external_changes();
    amp_models_sync_from_seed();
    if (s_selected_fx == FX_AMP) {
        if (amp_models_version() != s_amp_cards_version) build_model_cards(&g_effects[FX_AMP]);
        if (!lv_obj_has_flag(s_lib_backdrop, LV_OBJ_FLAG_HIDDEN) && amp_models_version() != s_lib_version) lib_fill();
        refresh_amp_status();
    }
    seed_link_stats_t s;
    seed_link_get_stats(&s);
    const bool live = s.connected && s.meters_valid;
    int32_t in = live ? meter_px(s.input_peak_dbfs) : 0;
    int32_t out = live ? meter_px(s.output_peak_dbfs) : 0;
    const bool mock_shown = !lv_obj_has_flag(s_meter_mock, LV_OBJ_FLAG_HIDDEN);
    if (live && s.peer_mock && !mock_shown) lv_obj_clear_flag(s_meter_mock, LV_OBJ_FLAG_HIDDEN);
    else if (!(live && s.peer_mock) && mock_shown) lv_obj_add_flag(s_meter_mock, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_height(s_meter_in) != in) lv_obj_set_height(s_meter_in, in);
    if (lv_obj_get_height(s_meter_out) != out) {
        lv_obj_set_height(s_meter_out, out);
        lv_color_t c = s.output_peak_dbfs > -1.0f ? UI_COLOR_MUTE
                     : s.output_peak_dbfs > -6.0f ? UI_COLOR_WARN : UI_COLOR_OK;
        lv_obj_set_style_bg_color(s_meter_out, c, 0);
    }
}

static lv_obj_t *make_meter(lv_obj_t *parent, lv_color_t color) {
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 8, METER_H);
    lv_obj_set_style_bg_color(track, UI_COLOR_INSET, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, 8, 0);
    lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(fill, color, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    return fill;
}

static void build_fx_panel(lv_obj_t *screen) {
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), 346);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_set_style_bg_color(panel, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main = lv_obj_create(panel);
    lv_obj_remove_style_all(main);
    lv_obj_set_height(main, LV_PCT(100));
    lv_obj_set_flex_grow(main, 1);
    lv_obj_set_style_pad_left(main, 32, 0);
    lv_obj_set_style_pad_right(main, 24, 0);
    lv_obj_set_style_pad_top(main, 18, 0);
    lv_obj_set_style_pad_bottom(main, 12, 0);
    lv_obj_set_style_pad_row(main, 12, 0);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(main, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_row = make_row(main, 14);
    s_fx_swatch = lv_obj_create(title_row);
    lv_obj_remove_style_all(s_fx_swatch);
    lv_obj_set_size(s_fx_swatch, 10, 10);
    lv_obj_set_style_radius(s_fx_swatch, 3, 0);
    lv_obj_set_style_bg_opa(s_fx_swatch, LV_OPA_COVER, 0);
    s_fx_title = make_label(title_row, "", &lv_font_montserrat_16, UI_COLOR_TEXT);
    s_fx_subtitle = make_label(title_row, "", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);

    s_model_row = lv_obj_create(main);
    lv_obj_remove_style_all(s_model_row);
    lv_obj_set_size(s_model_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_model_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_model_row, 10, 0);
    lv_obj_set_scroll_dir(s_model_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_model_row, LV_SCROLLBAR_MODE_OFF);
    s_amp_status = make_label(main, "", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_set_width(s_amp_status, LV_PCT(100));
    lv_label_set_long_mode(s_amp_status, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_amp_status, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *knob_area = make_row(main, 24);
    lv_obj_set_width(knob_area, LV_PCT(100));
    lv_obj_set_flex_grow(knob_area, 1);

    s_knob_row = lv_obj_create(knob_area);
    lv_obj_remove_style_all(s_knob_row);
    lv_obj_set_height(s_knob_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_knob_row, 1);
    lv_obj_set_flex_flow(s_knob_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_knob_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_knob_row, LV_OBJ_FLAG_SCROLLABLE);

    s_curve_col = lv_obj_create(knob_area);
    lv_obj_remove_style_all(s_curve_col);
    lv_obj_set_size(s_curve_col, 340, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_curve_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_curve_col, 8, 0);
    lv_obj_clear_flag(s_curve_col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_curve_caption = make_label(s_curve_col, "", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    ui_ir_curve_create(s_curve_col, 340, 150);

    // IN / OUT meters
    lv_obj_t *meters = lv_obj_create(panel);
    lv_obj_remove_style_all(meters);
    lv_obj_set_size(meters, 64, LV_PCT(100));
    lv_obj_set_style_border_side(meters, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(meters, 1, 0);
    lv_obj_set_style_border_color(meters, UI_COLOR_DIVIDER, 0);
    lv_obj_set_flex_flow(meters, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meters, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(meters, 10, 0);
    lv_obj_clear_flag(meters, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bars = make_row(meters, 7);
    s_meter_in = make_meter(bars, lv_color_hex(0x5c8ee0));
    s_meter_out = make_meter(bars, UI_COLOR_OK);
    lv_obj_t *names = make_row(meters, 8);
    make_label(names, "IN", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    make_label(names, "OUT", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    s_meter_mock = make_label(meters, "MOCK", &lv_font_montserrat_12, UI_COLOR_WARN);
    lv_obj_add_flag(s_meter_mock, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(meter_timer_cb, 50, NULL);

    rebuild_panel();
}

// ---------------------------------------------------------------------------------------------
// Bottom bar: Presets, Settings, master volume, mute.

static void presets_btn_cb(lv_event_t *e) { (void)e; lv_obj_clear_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN); }
static void settings_btn_cb(lv_event_t *e) { (void)e; lv_obj_clear_flag(s_settings_backdrop, LV_OBJ_FLAG_HIDDEN); }

static void refresh_master(void) {
    const bool muted = rig()->muted;
    int32_t shown = muted ? 0 : rig()->master_volume;
    lv_label_set_text_fmt(s_master_value, "%d%%", (int)shown);
    lv_obj_set_style_bg_color(s_mute_btn, muted ? UI_COLOR_MUTE : UI_COLOR_CARD, 0);
    lv_obj_set_style_border_color(s_mute_btn, muted ? UI_COLOR_MUTE : UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(s_mute_label, muted ? UI_COLOR_ON_ACCENT : UI_COLOR_TEXT, 0);
}

static void master_slider_cb(lv_event_t *e) {
    (void)e;
    rig_set_master_volume(lv_slider_get_value(s_master_slider), RIG_SRC_UI);
    rig_set_muted(false, RIG_SRC_UI);
    refresh_master();
}

static void mute_btn_cb(lv_event_t *e) {
    (void)e;
    // Mute leaves the master volume untouched, so un-muting restores the same level.
    const bool muted = !rig()->muted;
    rig_set_muted(muted, RIG_SRC_UI);
    lv_slider_set_value(s_master_slider, muted ? 0 : rig()->master_volume, LV_ANIM_ON);
    refresh_master();
}

static void build_bottom_bar(lv_obj_t *screen) {
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 82);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_pad_hor(bar, 28, 0);
    lv_obj_set_style_pad_column(bar, 22, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_ghost_btn(bar, "Presets", 110, presets_btn_cb);
    make_ghost_btn(bar, "Settings", 110, settings_btn_cb);

    lv_obj_t *master = make_row(bar, 16);
    lv_obj_set_flex_grow(master, 1);
    make_label(master, "MASTER", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    s_master_slider = lv_slider_create(master);
    lv_slider_set_range(s_master_slider, 0, 100);
    lv_slider_set_value(s_master_slider, rig()->master_volume, LV_ANIM_OFF);
    lv_obj_set_height(s_master_slider, 10);
    lv_obj_set_flex_grow(s_master_slider, 1);
    style_slider(s_master_slider);
    lv_obj_add_event_cb(s_master_slider, master_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_master_value = make_label(master, "", &lv_font_montserrat_14, UI_COLOR_TEXT);
    lv_obj_set_width(s_master_value, 44);

    s_mute_btn = make_ghost_btn(bar, "MUTE", 90, mute_btn_cb);
    s_mute_label = lv_obj_get_child(s_mute_btn, 0);
    refresh_master();
}

// ---------------------------------------------------------------------------------------------
// Settings card

static void close_settings_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_settings_backdrop, LV_OBJ_FLAG_HIDDEN);
    save_settings();
}

static void backlight_slider_cb(lv_event_t *e) {
    (void)e;
    board_set_backlight_pct((uint8_t)lv_slider_get_value(s_backlight_slider));
    lv_label_set_text_fmt(s_backlight_value, "%u%%", board_get_backlight_pct());
}

static void wifi_switch_cb(lv_event_t *e) {
    (void)e;
    s_wifi_on = !s_wifi_on;
    set_switch(s_wifi_switch, s_wifi_knob, s_wifi_on);
    refresh_status_dots();
    // TODO: actually enable/disable the Wi-Fi radio.
}

static void ble_switch_cb(lv_event_t *e) {
    (void)e;
    s_ble_on = !s_ble_on;
    set_switch(s_ble_switch, s_ble_knob, s_ble_on);
    refresh_status_dots();
    // TODO: actually enable/disable the BLE stack.
}

static lv_obj_t *make_setting_row(lv_obj_t *card, const char *text) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    make_label(row, text, NULL, UI_COLOR_TEXT);
    return row;
}

static void build_settings_overlay(lv_obj_t *screen) {
    s_settings_backdrop = make_backdrop(screen, close_settings_cb);
    lv_obj_t *card = make_card(s_settings_backdrop, 400, 290);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_row(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    make_close_btn(card, close_settings_cb);

    make_label(card, "Settings", &lv_font_montserrat_16, UI_COLOR_TEXT);
    make_label(card, "BACKLIGHT", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_t *bl_row = make_row(card, 14);
    lv_obj_set_width(bl_row, LV_PCT(100));
    s_backlight_slider = lv_slider_create(bl_row);
    lv_slider_set_range(s_backlight_slider, 10, 100);
    lv_slider_set_value(s_backlight_slider, board_get_backlight_pct(), LV_ANIM_OFF);
    lv_obj_set_height(s_backlight_slider, 10);
    lv_obj_set_flex_grow(s_backlight_slider, 1);
    style_slider(s_backlight_slider);
    lv_obj_add_event_cb(s_backlight_slider, backlight_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_backlight_value = make_label(bl_row, "", &lv_font_montserrat_14, UI_COLOR_TEXT);
    lv_obj_set_width(s_backlight_value, 44);
    lv_label_set_text_fmt(s_backlight_value, "%u%%", board_get_backlight_pct());

    lv_obj_t *wifi_row = make_setting_row(card, "Wi-Fi");
    s_wifi_switch = make_pill_switch(wifi_row, s_wifi_on, wifi_switch_cb, &s_wifi_knob);
    lv_obj_t *ble_row = make_setting_row(card, "Bluetooth");
    s_ble_switch = make_pill_switch(ble_row, s_ble_on, ble_switch_cb, &s_ble_knob);
}

// ---------------------------------------------------------------------------------------------
// Presets drawer (left), with the developer Test Mode entry at the bottom.

static void close_drawer_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void preset_cb(lv_event_t *e) {
    const ui_preset_t *p = &g_presets[(uintptr_t)lv_event_get_user_data(e)];
    lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
    apply_preset(p);
}

static void test_mode_btn_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_drawer_backdrop, LV_OBJ_FLAG_HIDDEN);
    ui_test_mode_open();
}

static void build_presets_drawer(lv_obj_t *screen) {
    s_drawer_backdrop = make_backdrop(screen, close_drawer_cb);

    lv_obj_t *drawer = lv_obj_create(s_drawer_backdrop);
    lv_obj_set_size(drawer, 300, LV_PCT(100));
    lv_obj_align(drawer, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(drawer, UI_COLOR_PANEL, 0);
    lv_obj_set_style_radius(drawer, 0, 0);
    lv_obj_set_style_border_side(drawer, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(drawer, UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_hor(drawer, 0, 0);
    lv_obj_set_style_pad_ver(drawer, 18, 0);
    lv_obj_set_style_pad_row(drawer, 0, 0);
    lv_obj_set_flex_flow(drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(drawer, LV_OBJ_FLAG_CLICKABLE); // absorbs taps so they don't close the drawer

    lv_obj_t *head = lv_obj_create(drawer);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), 44);
    lv_obj_set_style_pad_left(head, 22, 0);
    lv_obj_set_style_pad_right(head, 10, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, UI_COLOR_DIVIDER, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *title = make_label(head, "Presets", &lv_font_montserrat_16, UI_COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, -6);
    lv_obj_t *x = lv_button_create(head);
    lv_obj_set_size(x, 40, 40);
    lv_obj_set_style_bg_opa(x, 0, 0);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, -6);
    lv_obj_add_event_cb(x, close_drawer_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(x, LV_SYMBOL_CLOSE, NULL, UI_COLOR_TEXT_MUTED));

    for (uintptr_t i = 0; i < UI_PRESET_COUNT; i++) {
        lv_obj_t *b = lv_obj_create(drawer);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_PCT(100), 62);
        lv_obj_set_style_pad_hor(b, 22, 0);
        lv_obj_set_style_pad_ver(b, 12, 0);
        lv_obj_set_style_pad_row(b, 3, 0);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, UI_COLOR_DIVIDER, 0);
        lv_obj_set_style_bg_color(b, UI_COLOR_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void *)i);
        s_preset_names[i] = make_label(b, g_presets[i].name, &lv_font_montserrat_14, UI_COLOR_TEXT);
        make_label(b, g_presets[i].tagline, &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    }
    refresh_preset_names();

    lv_obj_t *spacer = lv_obj_create(drawer);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *dev = lv_obj_create(drawer);
    lv_obj_remove_style_all(dev);
    lv_obj_set_size(dev, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(dev, 22, 0);
    lv_obj_set_style_pad_top(dev, 14, 0);
    lv_obj_set_style_pad_row(dev, 8, 0);
    lv_obj_set_style_border_side(dev, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(dev, 1, 0);
    lv_obj_set_style_border_color(dev, UI_COLOR_DIVIDER, 0);
    lv_obj_set_flex_flow(dev, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(dev, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    make_label(dev, "DEVELOPER", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);

    lv_obj_t *test_btn = lv_button_create(dev);
    lv_obj_set_size(test_btn, LV_PCT(100), 48);
    lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x372a2b), 0);
    lv_obj_set_style_border_color(test_btn, lv_color_hex(0x5a3c3c), 0);
    lv_obj_set_style_border_width(test_btn, 1, 0);
    lv_obj_set_style_radius(test_btn, 10, 0);
    lv_obj_add_event_cb(test_btn, test_mode_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *test_l = make_label(test_btn, "Test Mode", NULL, UI_COLOR_MUTE);
    lv_obj_align(test_l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *test_sub = make_label(test_btn, "ESP32 + SEED3", &lv_font_montserrat_12, UI_COLOR_TEXT_DIM);
    lv_obj_align(test_sub, LV_ALIGN_RIGHT_MID, -4, 0);
}

// ---------------------------------------------------------------------------------------------
// Add / remove effects catalog

static void refresh_catalog(void) {
    for (int fx = 0; fx < UI_CHAIN_MAX; fx++) {
        const bool in = s_chips[fx] != NULL;
        lv_obj_t *pill = s_catalog_pills[fx];
        lv_label_set_text(pill, in ? "IN CHAIN" : "ADD");
        lv_obj_set_style_bg_color(pill, in ? g_effects[fx].color : UI_COLOR_TRACK, 0);
        lv_obj_set_style_text_color(pill, in ? UI_COLOR_ON_ACCENT : UI_COLOR_TEXT_DIM, 0);
    }
}

static void catalog_open_cb(lv_event_t *e) {
    (void)e;
    refresh_catalog();
    lv_obj_clear_flag(s_catalog_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void catalog_close_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_catalog_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void catalog_toggle_cb(lv_event_t *e) {
    uint8_t fx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t chain[UI_CHAIN_MAX];
    uint8_t len = rig()->chain_len;
    memcpy(chain, rig()->chain, sizeof(chain));
    int idx = -1;
    for (int i = 0; i < len; i++) if (chain[i] == fx) idx = i;

    if (idx >= 0) {
        if (len <= 1) return; // keep at least one effect in the chain
        memmove(&chain[idx], &chain[idx + 1], (size_t)(len - idx - 1));
        len--;
        rig_set_fx_on(fx, false, RIG_SRC_UI);
        if (s_selected_fx == fx) s_selected_fx = chain[0];
    } else {
        if (len >= UI_CHAIN_MAX) return;
        chain[len++] = fx;
    }
    rig_set_chain(chain, len, RIG_SRC_UI);
    rebuild_rail();
    rebuild_panel();
    refresh_catalog();
    ui_main_mark_dirty();
}


// ---------------------------------------------------------------------------------------------
// SD Library: every amp profile on the card, full width, with its status. The AMP card row only
// holds the built-in amps, the active SD profile and the card that opens this.


static lv_obj_t *s_lib_backdrop, *s_lib_list, *s_lib_note;
static uint32_t s_lib_version;

static void lib_close_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_lib_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void lib_row_cb(lv_event_t *e) {
    const int index = (int)(uintptr_t)lv_event_get_user_data(e);
    amp_model_t am;
    if (!amp_models_get(index, &am) || am.rejected) return; // its row says why
    rig_set_amp_sd(am.sd_hash, RIG_SRC_UI); // the link uploads it if the Seed lacks it
    ui_main_mark_dirty();
    lv_obj_add_flag(s_lib_backdrop, LV_OBJ_FLAG_HIDDEN);
    // Rebuilding the AMP cards here is safe: the clicked row belongs to the popup, not the row.
    build_model_cards(&g_effects[FX_AMP]);
    refresh_amp_status();
}

static void lib_rescan_cb(lv_event_t *e) {
    (void)e;
    nam_store_rescan(); // the list refreshes when the scan publishes (meter_timer_cb)
    lv_label_set_text(s_lib_note, "Reading the SD card...");
}

static void lib_fill(void) {
    lv_obj_clean(s_lib_list);
    seed_link_stats_t s;
    seed_link_get_stats(&s);
    const uint32_t selected = rig()->amp_sd_hash;
    const int first = amp_models_builtin_count(), n = amp_models_count();
    for (int i = first; i < n; i++) {
        amp_model_t am;
        if (!amp_models_get(i, &am)) break;
        const bool sel = !am.rejected && am.sd_hash == selected;
        lv_obj_t *row = lv_obj_create(s_lib_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_row(row, 3, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, UI_COLOR_RAISED, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, sel ? g_effects[FX_AMP].color : UI_COLOR_RAISED, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (!am.rejected) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, UI_COLOR_PRESSED, LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, lib_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        }
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        lv_obj_t *name = make_label(row, am.name, &lv_font_montserrat_14,
                                    am.rejected ? UI_COLOR_TEXT_MUTED : sel ? g_effects[FX_AMP].color : UI_COLOR_TEXT);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        char sub[160];
        if (am.rejected) {
            snprintf(sub, sizeof(sub), "%s  -  %s", am.file, am.desc);
        } else {
            const bool running = sel && s.connected && s.active_kind == SLP_MODEL_UPLOADED && s.active_hash == am.sd_hash;
            snprintf(sub, sizeof(sub), "%s  -  %s", am.file,
                     running ? (s.peer_mock ? "On the Seed (MOCK)" : "On the Seed") : sel ? "Selected" : "Ready, tap to use");
        }
        lv_obj_t *d = make_label(row, sub, &lv_font_montserrat_12, am.rejected ? UI_COLOR_WARN : UI_COLOR_TEXT_MUTED);
        lv_obj_set_width(d, LV_PCT(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    }
    nam_store_status_t st;
    nam_store_status(&st);
    lv_label_set_text(s_lib_note, n > first ? st.status
                                            : st.card_ok ? "No .nam files in the /nam folder. Copy TONE3000 A2 downloads there on a PC."
                                                         : st.status);
    s_lib_version = amp_models_version();
}

static void lib_open(void) {
    lib_fill();
    lv_obj_clear_flag(s_lib_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void build_sd_library(lv_obj_t *screen) {
    s_lib_backdrop = make_backdrop(screen, lib_close_cb);
    lv_obj_t *card = make_card(s_lib_backdrop, 680, 520);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    make_close_btn(card, lib_close_cb);
    make_label(card, "SD Library - amp profiles", &lv_font_montserrat_16, UI_COLOR_TEXT);

    s_lib_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_lib_list);
    lv_obj_set_width(s_lib_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_lib_list, 1);
    lv_obj_set_flex_flow(s_lib_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lib_list, 8, 0);
    lv_obj_set_scroll_dir(s_lib_list, LV_DIR_VER);

    lv_obj_t *foot = make_row(card, 14);
    lv_obj_set_width(foot, LV_PCT(100));
    s_lib_note = make_label(foot, "", &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_set_flex_grow(s_lib_note, 1);
    lv_label_set_long_mode(s_lib_note, LV_LABEL_LONG_WRAP);
    make_ghost_btn(foot, "Rescan SD card", 170, lib_rescan_cb);
}

static void build_catalog(lv_obj_t *screen) {
    s_catalog_backdrop = make_backdrop(screen, catalog_close_cb);
    lv_obj_t *card = make_card(s_catalog_backdrop, 530, 452);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    make_close_btn(card, catalog_close_cb);
    make_label(card, "Add / Remove Effects", &lv_font_montserrat_16, UI_COLOR_TEXT);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (uintptr_t fx = 0; fx < UI_CHAIN_MAX; fx++) {
        lv_obj_t *item = lv_obj_create(grid);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, 236, 46); // two columns + 8 px gap = the card's 480 px content width
        lv_obj_set_style_bg_color(item, UI_COLOR_RAISED, 0);
        lv_obj_set_style_bg_color(item, UI_COLOR_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 10, 0);
        lv_obj_set_style_pad_hor(item, 14, 0);
        lv_obj_set_style_pad_column(item, 12, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(item, catalog_toggle_cb, LV_EVENT_CLICKED, (void *)fx);

        lv_obj_t *sw = lv_obj_create(item);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 9, 9);
        lv_obj_set_style_radius(sw, 3, 0);
        lv_obj_set_style_bg_color(sw, g_effects[fx].color, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *name = make_label(item, g_effects[fx].label, NULL, UI_COLOR_TEXT);
        lv_obj_set_flex_grow(name, 1);

        lv_obj_t *pill = make_label(item, "", &lv_font_montserrat_12, UI_COLOR_TEXT_DIM);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 9, 0);
        lv_obj_set_style_pad_ver(pill, 4, 0);
        lv_obj_set_style_radius(pill, 6, 0);
        s_catalog_pills[fx] = pill;
    }

    lv_obj_t *hint = make_label(card, "Tap outside to close. CAB stays pinned at the end of the chain.",
                                &lv_font_montserrat_12, UI_COLOR_TEXT_MUTED);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------------------------
// Following changes made elsewhere (physical knobs, SD loader)

uint8_t ui_main_selected_fx(void) { return s_selected_fx; }

// rig_state calls this from the knob (or SD) task after a change. It only records what changed:
// touching LVGL from another task would mean taking the LVGL lock up to 50 times a second, which
// starves the diag watchdog's lock probe (it then wrongly restarts the RGB scan-out). The LVGL
// task applies the changes from meter_timer_cb instead.
static uint32_t s_external_dirty;

static void on_rig_external(uint32_t dirty, rig_src_t src) {
    (void)src;
    __atomic_fetch_or(&s_external_dirty, dirty, __ATOMIC_RELAXED);
}

// LVGL task only. Widgets are updated with LVGL's set-value calls, which emit no VALUE_CHANGED
// events, so nothing echoes back into rig_state as a second change.
static void apply_external_changes(void) {
    const uint32_t dirty = __atomic_exchange_n(&s_external_dirty, 0, __ATOMIC_RELAXED);
    if (!dirty) return;
    if (dirty & RIG_DIRTY_MASTER) {
        lv_slider_set_value(s_master_slider, rig()->muted ? 0 : rig()->master_volume, LV_ANIM_OFF);
        refresh_master();
    }
    if (dirty & RIG_DIRTY_PARAMS) {
        const uint8_t n = g_effects[s_selected_fx].knob_count;
        for (uint8_t k = 0; k < n && k < lv_obj_get_child_cnt(s_knob_row); k++) {
            ui_knob_set_value(lv_obj_get_child(s_knob_row, k), g_knob_values[s_selected_fx][k]);
        }
        if (s_selected_fx == FX_CAB) update_cab_curve();
        ui_vertical_slider_refresh();
    }
    if (dirty & RIG_DIRTY_FX) {
        refresh_all_chip_styles();
        restyle_model_cards(s_selected_fx);
    }
    if (dirty & RIG_DIRTY_CHAIN) rebuild_rail();
    // Master volume doesn't mark the rig EDITED on the touch path either.
    if (dirty & (RIG_DIRTY_PARAMS | RIG_DIRTY_FX | RIG_DIRTY_CHAIN)) ui_main_mark_dirty();
}

// ---------------------------------------------------------------------------------------------
// Theme + init

static lv_style_t s_text_style;

static void app_theme_apply_cb(lv_theme_t *th, lv_obj_t *obj) {
    (void)th;
    // Labels are skipped so they keep inheriting the text colour of their container.
    if (!lv_obj_check_type(obj, &lv_label_class)) lv_obj_add_style(obj, &s_text_style, 0);
}

// LVGL's default theme in dark mode, extended so every widget's text uses the
// warm UI_COLOR_TEXT. Done at runtime rather than via CONFIG_LV_THEME_DEFAULT_DARK
// because sdkconfig is gitignored and an existing one ignores sdkconfig.defaults.
// Must run before any widget is created - themes apply at creation time.
static void init_theme(void) {
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *base = lv_theme_default_init(disp, UI_COLOR_ACCENT, UI_COLOR_MUTE, true, LV_FONT_DEFAULT);

    lv_style_init(&s_text_style);
    lv_style_set_text_color(&s_text_style, UI_COLOR_TEXT);

    lv_theme_t *app = lv_theme_create(); // lives for the life of the display
    lv_theme_copy(app, base);
    lv_theme_set_parent(app, base);
    lv_theme_set_apply_cb(app, app_theme_apply_cb);
    lv_display_set_theme(disp, app);
}

void ui_main_init(void) {
    init_theme();
    amp_models_init();
    rig_init();
    load_saved_state();

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    // remove_style_all() also drops the theme's text colour, which would leave every
    // label below inheriting LVGL's built-in default: black.
    lv_obj_set_style_text_color(screen, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(screen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(screen);
    build_chain_rail(screen);
    build_fx_panel(screen);
    build_bottom_bar(screen);
    ui_vertical_slider_init(screen);
    build_settings_overlay(screen);
    build_presets_drawer(screen);
    build_catalog(screen);
    build_sd_library(screen);
    ui_test_mode_init(screen);

    lv_screen_load(screen);
    rig_set_observer(on_rig_external);
}
