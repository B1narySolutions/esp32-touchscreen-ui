#include "ui_effect_chip.h"
#include "ui_effects_data.h"
#include "ui_theme.h"
#include "ui_main.h"
#include <stdlib.h>

#define DRAG_THRESHOLD_PX 12

typedef struct {
    lv_obj_t *dot;
    lv_obj_t *label;
    lv_obj_t *state_text;
} chip_children_t;

static lv_point_t s_press_start;
static bool s_dragging = false;

static void chip_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *chip = lv_event_get_target(e);
    uint8_t fx_index = (uint8_t)(uintptr_t)lv_obj_get_user_data(chip);
    lv_indev_t *indev = lv_indev_get_act();

    if (code == LV_EVENT_PRESSED) {
        if (indev) lv_indev_get_point(indev, &s_press_start);
        s_dragging = false;

    } else if (code == LV_EVENT_PRESSING) {
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);

        if (!s_dragging && LV_ABS(p.x - s_press_start.x) > DRAG_THRESHOLD_PX) {
            s_dragging = true;
        }
        if (s_dragging) {
            lv_obj_t *rail = lv_obj_get_parent(chip);
            uint32_t n = lv_obj_get_child_cnt(rail);
            for (uint32_t i = 0; i < n; i++) {
                lv_obj_t *sib = lv_obj_get_child(rail, i);
                if (sib == chip) continue;
                lv_area_t a;
                lv_obj_get_coords(sib, &a);
                if (p.x >= a.x1 && p.x <= a.x2) {
                    lv_obj_move_to_index(chip, i);
                    ui_main_on_chip_reordered(rail);
                    break;
                }
            }
        }

    } else if (code == LV_EVENT_CLICKED) {
        // LVGL does not send CLICKED for a press that moved past its own drag
        // threshold, but our own s_dragging flag (independent, coarser threshold)
        // is the authority here since we manually moved the object ourselves.
        if (s_dragging) {
            s_dragging = false;
            return;
        }
        ui_main_on_chip_tapped(fx_index);

    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_dragging = false;
    }
}

lv_obj_t *ui_effect_chip_create(lv_obj_t *parent, uint8_t fx_index) {
    const ui_effect_def_t *def = &g_effects[fx_index];

    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_size(chip, UI_CHIP_W, UI_CHIP_H);
    lv_obj_set_style_bg_color(chip, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 14, 0);
    lv_obj_set_style_border_width(chip, 2, 0);
    lv_obj_set_style_border_color(chip, UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE); // plain lv_obj isn't clickable by default
    lv_obj_set_user_data(chip, (void *)(uintptr_t)fx_index);

    lv_obj_t *grip = lv_label_create(chip);
    lv_label_set_text(grip, LV_SYMBOL_LIST); // stand-in drag-handle glyph; swap for a
                                              // custom "grip dots" icon font if desired
    lv_obj_set_style_text_color(grip, lv_color_hex(0x454349), 0);
    lv_obj_align(grip, LV_ALIGN_TOP_LEFT, 6, 4);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, def->label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *row = lv_obj_create(chip);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 16);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *state_text = lv_label_create(row);
    lv_obj_set_style_text_font(state_text, &lv_font_montserrat_10, 0);

    // Stash the sub-widgets we need to restyle later directly on the chip, so
    // ui_effect_chip_refresh() doesn't have to know the child layout.
    chip_children_t *kids = lv_malloc(sizeof(chip_children_t));
    kids->dot = dot;
    kids->label = label;
    kids->state_text = state_text;
    lv_obj_set_user_data(row, kids); // freed implicitly with the object tree at teardown

    lv_obj_add_event_cb(chip, chip_event_cb, LV_EVENT_ALL, NULL);

    return chip;
}

void ui_effect_chip_refresh(lv_obj_t *chip, bool is_on, bool is_selected) {
    uint8_t fx_index = (uint8_t)(uintptr_t)lv_obj_get_user_data(chip);
    const ui_effect_def_t *def = &g_effects[fx_index];

    lv_obj_t *row = lv_obj_get_child(chip, 2); // grip(0), label(1), row(2) - see create()
    chip_children_t *kids = (chip_children_t *)lv_obj_get_user_data(row);

    lv_obj_set_style_border_color(chip, is_selected ? def->color : UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(kids->label, (is_on || is_selected) ? UI_COLOR_TEXT : lv_color_hex(0x6a6870), 0);
    lv_obj_set_style_bg_color(kids->dot, is_on ? def->color : lv_color_hex(0x4a4850), 0);
    lv_label_set_text(kids->state_text, is_on ? "ON" : "BYPASS");
    lv_obj_set_style_text_color(kids->state_text, is_on ? UI_COLOR_TEXT_DIM : lv_color_hex(0x55535a), 0);
}
