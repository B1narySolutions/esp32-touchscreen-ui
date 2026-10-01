#include "ui_ir_curve.h"
#include "ui_effects_data.h"
#include "ui_theme.h"
#include <math.h>

#define CURVE_POINTS 81  // same resolution as the preview's irCurve()
#define DB_TOP       10.0f
#define DB_RANGE     46.0f

static lv_obj_t *s_box;
static lv_obj_t *s_line;
static lv_point_precise_t s_pts[CURVE_POINTS];
static int32_t s_w, s_h;           // plot area (content box)
static float s_zero_y;
static lv_color_t s_color;

static const float k_grid_hz[] = { 100, 1000, 10000 };
static const char *k_grid_txt[] = { "100", "1k", "10k" };

static float x_of(float hz) {
    const float lo = log10f(20), span = log10f(20000) - lo;
    return (log10f(hz) - lo) / span * s_w;
}

static float y_of(float db) {
    float y = (DB_TOP - db) / DB_RANGE * s_h;
    return y < 0 ? 0 : (y > s_h ? s_h : y);
}

// Area under the curve, grid and 0 dB line - drawn under the lv_line child.
static void draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_content_coords(s_box, &c);

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = s_color;
    fill.opa = 33; // ~13 %, as in the preview
    for (int32_t x = 0; x < s_w; x += 2) {
        float fi = (float)x / s_w * (CURVE_POINTS - 1);
        int i = (int)fi;
        float t = fi - i;
        float y = s_pts[i].y + (i + 1 < CURVE_POINTS ? (s_pts[i + 1].y - s_pts[i].y) * t : 0);
        lv_area_t col = { c.x1 + x, c.y1 + (int32_t)y, c.x1 + x + 1, c.y2 };
        if (col.y1 <= col.y2) lv_draw_fill(layer, &fill, &col);
    }

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = UI_COLOR_DIVIDER;
    ln.width = 1;
    for (unsigned i = 0; i < sizeof(k_grid_hz) / sizeof(k_grid_hz[0]); i++) {
        float x = c.x1 + x_of(k_grid_hz[i]);
        ln.p1 = (lv_point_precise_t){ x, c.y1 };
        ln.p2 = (lv_point_precise_t){ x, c.y2 };
        lv_draw_line(layer, &ln);
    }
    ln.color = UI_COLOR_BORDER;
    ln.dash_width = 3;
    ln.dash_gap = 4;
    ln.p1 = (lv_point_precise_t){ c.x1, c.y1 + s_zero_y };
    ln.p2 = (lv_point_precise_t){ c.x2, c.y1 + s_zero_y };
    lv_draw_line(layer, &ln);
}

lv_obj_t *ui_ir_curve_create(lv_obj_t *parent, int32_t w, int32_t h) {
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, w, h);
    lv_obj_set_style_bg_color(s_box, UI_COLOR_INSET, 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_box, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_border_width(s_box, 1, 0);
    lv_obj_set_style_radius(s_box, 10, 0);
    lv_obj_set_style_clip_corner(s_box, true, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_box, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    s_w = w - 2;
    s_h = h - 2;

    s_line = lv_line_create(s_box);
    lv_obj_set_pos(s_line, 0, 0);
    lv_obj_set_size(s_line, s_w, s_h);
    lv_obj_set_style_line_width(s_line, 2, 0);
    lv_obj_set_style_line_rounded(s_line, true, 0);

    for (unsigned i = 0; i < sizeof(k_grid_hz) / sizeof(k_grid_hz[0]); i++) {
        lv_obj_t *l = lv_label_create(s_box);
        lv_label_set_text(l, k_grid_txt[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_pos(l, (int32_t)x_of(k_grid_hz[i]) + 4, s_h - 18);
    }
    return s_box;
}

void ui_ir_curve_update(uint8_t ir_index, int32_t lowcut, int32_t highcut, lv_color_t color) {
    const ui_ir_shape_t *p = &g_ir_shapes[ir_index];
    const float user_lo = 20.0f * powf(15.0f, lowcut / 100.0f);
    const float user_hi = 3000.0f * powf(6.5f, highcut / 100.0f);
    const float lo = log10f(20), span = log10f(20000) - lo;

    for (int i = 0; i < CURVE_POINTS; i++) {
        float f = powf(10.0f, lo + span * i / (CURVE_POINTS - 1));
        float d = -10 * log10f(1 + powf(p->lo_hz / f, 4)) - 10 * log10f(1 + powf(f / p->hi_hz, 6));
        d += -10 * log10f(1 + powf(user_lo / f, 4)) - 10 * log10f(1 + powf(f / user_hi, 6));
        for (int k = 0; k < p->peak_count; k++) {
            float o = log2f(f / p->peaks[k].f) / p->peaks[k].w;
            d += p->peaks[k].g * expf(-0.5f * o * o);
        }
        s_pts[i].x = (float)i / (CURVE_POINTS - 1) * s_w;
        s_pts[i].y = y_of(d);
    }
    s_zero_y = y_of(0);
    s_color = color;
    lv_obj_set_style_line_color(s_line, color, 0);
    lv_line_set_points(s_line, s_pts, CURVE_POINTS);
    lv_obj_invalidate(s_box);
}
