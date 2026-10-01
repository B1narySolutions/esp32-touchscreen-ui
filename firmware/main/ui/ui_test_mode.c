#include "ui_test_mode.h"
#include "ui_theme.h"
#include "diag.h"
#include "seed_link.h"
#include <math.h>
#include <stdio.h>

#define ESP_ROWS  11
#define SEED_ROWS 12

static lv_obj_t *s_root;
static lv_obj_t *s_bars;
static lv_obj_t *s_esp_vals[ESP_ROWS];
static lv_obj_t *s_seed_vals[SEED_ROWS];
static lv_obj_t *s_seed_banner;
static lv_obj_t *s_seed_dot;
static lv_obj_t *s_ping_btn;
static lv_timer_t *s_timer;

static const char *k_esp_keys[ESP_ROWS] = {
    "Uptime", "Reset reason", "Free internal heap", "Min free heap (since boot)",
    "Largest free block", "Free PSRAM", "Chip temperature", "Battery",
    "Backlight PWM", "UI redraws / s", "Touch",
};
static const char *k_seed_keys[SEED_ROWS] = {
    "Link", "Packets TX / RX", "Link errors", "Round-trip latency", "DSP firmware",
    "Sample rate / block", "DSP CPU load", "NAM model", "Input peak", "Output peak",
    "Clips since boot", "Bus",
};

static lv_obj_t *make_panel(lv_obj_t *parent, const char *title, const char *sub,
                            const char **keys, int n, lv_obj_t **vals, lv_obj_t **dot_out) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_height(p, LV_PCT(100));
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x16161a), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x26262c), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 14, 0);
    lv_obj_set_style_pad_row(p, 2, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = lv_obj_create(p);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_style_pad_bottom(head, 6, 0);
    lv_obj_t *dot = lv_obj_create(head);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, UI_COLOR_OK, 0);
    if (dot_out) *dot_out = dot;
    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_t *st = lv_label_create(head);
    lv_label_set_text(st, sub);
    lv_obj_set_style_text_color(st, lv_color_hex(0x5a5852), 0);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_10, 0);

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x1f1f24), 0);
        lv_obj_t *k = lv_label_create(row);
        lv_label_set_text(k, keys[i]);
        lv_obj_set_style_text_color(k, lv_color_hex(0x8f8b82), 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_12, 0);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *v = lv_label_create(row);
        lv_label_set_text(v, "-");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_12, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
        vals[i] = v;
    }
    return p;
}

static void set_dash(lv_obj_t *l) { lv_label_set_text(l, "-"); }

static void refresh(lv_timer_t *t) {
    (void)t;
    diag_snapshot_t d;
    diag_take(&d);

    lv_label_set_text_fmt(s_esp_vals[0], "%02lu:%02lu:%02lu", (unsigned long)(d.uptime_s / 3600),
                          (unsigned long)(d.uptime_s / 60 % 60), (unsigned long)(d.uptime_s % 60));
    lv_label_set_text(s_esp_vals[1], d.reset_reason);
    lv_label_set_text_fmt(s_esp_vals[2], "%lu KB", (unsigned long)(d.heap_free / 1024));
    lv_label_set_text_fmt(s_esp_vals[3], "%lu KB", (unsigned long)(d.heap_min_free / 1024));
    lv_label_set_text_fmt(s_esp_vals[4], "%lu KB", (unsigned long)(d.heap_largest_block / 1024));
    lv_label_set_text_fmt(s_esp_vals[5], "%.2f MB of %.2f MB", d.psram_free / 1048576.0, d.psram_total / 1048576.0);
    if (isnan(d.chip_temp_c)) lv_label_set_text(s_esp_vals[6], "sensor unavailable");
    else lv_label_set_text_fmt(s_esp_vals[6], "%.1f C", d.chip_temp_c);
    if (d.battery_v < 0) lv_label_set_text(s_esp_vals[7], "read failed");
    else lv_label_set_text_fmt(s_esp_vals[7], "%.2f V", d.battery_v);
    lv_label_set_text_fmt(s_esp_vals[8], "%u %%", d.backlight_pct);
    lv_label_set_text_fmt(s_esp_vals[9], "%lu", (unsigned long)d.redraws_per_s);
    if (d.last_touch_x < 0) lv_label_set_text_fmt(s_esp_vals[10], "%lu taps", (unsigned long)d.touches);
    else lv_label_set_text_fmt(s_esp_vals[10], "%lu taps, last (%d, %d)", (unsigned long)d.touches,
                               d.last_touch_x, d.last_touch_y);

    seed_link_stats_t s;
    seed_link_get_stats(&s);
    lv_obj_set_style_bg_color(s_seed_dot, s.connected ? UI_COLOR_OK : UI_COLOR_MUTE, 0);
    if (s.connected) lv_obj_add_flag(s_seed_banner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_seed_banner, LV_OBJ_FLAG_HIDDEN);
    if (s.connected) lv_obj_clear_state(s_ping_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ping_btn, LV_STATE_DISABLED);

    lv_label_set_text(s_seed_vals[0], s.connected ? "Connected" : "Not connected");
    lv_label_set_text(s_seed_vals[11], "not chosen yet");
    if (!s.connected) {
        for (int i = 1; i < SEED_ROWS - 1; i++) set_dash(s_seed_vals[i]);
        return;
    }
    lv_label_set_text_fmt(s_seed_vals[1], "%lu / %lu", (unsigned long)s.packets_tx, (unsigned long)s.packets_rx);
    lv_label_set_text_fmt(s_seed_vals[2], "%lu", (unsigned long)s.link_errors);
    lv_label_set_text_fmt(s_seed_vals[3], "%.2f ms", s.rtt_ms);
    lv_label_set_text(s_seed_vals[4], s.dsp_fw_version);
    lv_label_set_text_fmt(s_seed_vals[5], "%lu Hz / %u", (unsigned long)s.sample_rate_hz, s.block_size);
    lv_label_set_text_fmt(s_seed_vals[6], "%.1f %%", s.dsp_cpu_pct);
    lv_label_set_text(s_seed_vals[7], s.nam_model);
    lv_label_set_text_fmt(s_seed_vals[8], "%.1f dBFS", s.input_peak_dbfs);
    lv_label_set_text_fmt(s_seed_vals[9], "%.1f dBFS", s.output_peak_dbfs);
    lv_label_set_text_fmt(s_seed_vals[10], "%lu", (unsigned long)s.clip_count);
}

static void touch_pad_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    diag_note_touch(p.x, p.y);
    refresh(NULL);
    (void)e;
}

static void exit_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_timer);
}

static void bars_open_cb(lv_event_t *e) { (void)e; lv_obj_clear_flag(s_bars, LV_OBJ_FLAG_HIDDEN); }
static void bars_close_cb(lv_event_t *e) { (void)e; lv_obj_add_flag(s_bars, LV_OBJ_FLAG_HIDDEN); }
static void ping_cb(lv_event_t *e) { (void)e; seed_link_ping(); refresh(NULL); }
static void reset_cb(lv_event_t *e) { (void)e; diag_reset_touches(); seed_link_reset_counters(); refresh(NULL); }

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_color_t bg, lv_color_t fg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}

void ui_test_mode_init(lv_obj_t *screen) {
    s_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x0e0e11), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_root, UI_COLOR_TEXT, 0);
    lv_obj_set_style_pad_hor(s_root, 22, 0);
    lv_obj_set_style_pad_ver(s_root, 16, 0);
    lv_obj_set_style_pad_row(s_root, 12, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(s_root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), 40);
    lv_obj_t *pill = lv_label_create(head);
    lv_label_set_text(pill, "TEST MODE");
    lv_obj_set_style_bg_color(pill, UI_COLOR_MUTE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x101013), 0);
    lv_obj_set_style_text_font(pill, &lv_font_montserrat_10, 0);
    lv_obj_set_style_pad_hor(pill, 8, 0);
    lv_obj_set_style_pad_ver(pill, 4, 0);
    lv_obj_set_style_radius(pill, 5, 0);
    lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, "Diagnostics - live hardware readings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 96, 0);
    lv_obj_t *exit_btn = make_btn(head, "Exit Test Mode", exit_cb, UI_COLOR_ACCENT, lv_color_hex(0x161409));
    lv_obj_align(exit_btn, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 12, 0);

    make_panel(body, "ESP32-S3", "UI BOARD", k_esp_keys, ESP_ROWS, s_esp_vals, NULL);
    lv_obj_t *seed = make_panel(body, "SEED3 - NAM", "DSP BOARD", k_seed_keys, SEED_ROWS, s_seed_vals, &s_seed_dot);
    s_seed_banner = lv_label_create(seed);
    lv_obj_move_to_index(s_seed_banner, 1);
    lv_obj_set_width(s_seed_banner, LV_PCT(100));
    lv_label_set_long_mode(s_seed_banner, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_seed_banner, "No link to the Daisy Seed yet. These fields fill in once the "
                                     "ESP32 <-> SEED3 link is wired and its protocol is implemented.");
    lv_obj_set_style_text_font(s_seed_banner, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_seed_banner, lv_color_hex(0xe2a05c), 0);
    lv_obj_set_style_bg_color(s_seed_banner, lv_color_hex(0x231d16), 0);
    lv_obj_set_style_bg_opa(s_seed_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_seed_banner, 8, 0);
    lv_obj_set_style_radius(s_seed_banner, 8, 0);

    lv_obj_t *side = lv_obj_create(body);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, 230, LV_PCT(100));
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 10, 0);

    lv_obj_t *pad = lv_obj_create(side);
    lv_obj_set_width(pad, LV_PCT(100));
    lv_obj_set_flex_grow(pad, 1);
    lv_obj_set_style_bg_color(pad, lv_color_hex(0x121215), 0);
    lv_obj_set_style_border_color(pad, lv_color_hex(0x33333b), 0);
    lv_obj_set_style_border_width(pad, 1, 0);
    lv_obj_set_style_radius(pad, 14, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pad, touch_pad_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *pad_l = lv_label_create(pad);
    lv_label_set_text(pad_l, "TOUCH TEST - tap here");
    lv_obj_set_style_text_font(pad_l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(pad_l, lv_color_hex(0x5a5852), 0);
    lv_obj_align(pad_l, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *btns = lv_obj_create(side);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(btns, 8, 0);
    make_btn(btns, "Color bars", bars_open_cb, UI_COLOR_CARD, UI_COLOR_TEXT);
    s_ping_btn = make_btn(btns, "Ping SEED", ping_cb, UI_COLOR_CARD, UI_COLOR_TEXT);
    make_btn(btns, "Reset counters", reset_cb, UI_COLOR_CARD, UI_COLOR_TEXT);

    s_bars = lv_obj_create(screen);
    lv_obj_remove_style_all(s_bars);
    lv_obj_set_size(s_bars, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_bars, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(s_bars, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_bars, bars_close_cb, LV_EVENT_CLICKED, NULL);
    static const uint32_t bar_colors[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *b = lv_obj_create(s_bars);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, LV_PCT(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, lv_color_hex(bar_colors[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }

    s_timer = lv_timer_create(refresh, 1000, NULL);
    lv_timer_pause(s_timer);
}

void ui_test_mode_open(void) {
    lv_obj_move_foreground(s_root);
    lv_obj_move_foreground(s_bars);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    refresh(NULL);
    lv_timer_resume(s_timer);
}
