#include "ui_effect_chip.h"
#include "ui_effects_data.h"
#include "ui_theme.h"
#include "ui_main.h"

// Reorder interaction: the rail scrolls horizontally, so a plain swipe across chips scrolls it.
// Holding a chip (LVGL long-press, 400 ms) picks it up: the rail stops scrolling, the chip
// follows the finger to a new slot, and dragging near either edge auto-scrolls the rail.
#define EDGE_SCROLL_ZONE_PX 56
#define EDGE_SCROLL_STEP_PX 14

// Child layout, relied on by ui_effect_chip_refresh(): tag(0), label(1), row(2) -> dot, state
enum { CHILD_TAG, CHILD_LABEL, CHILD_ROW };

static bool s_dragging;
static bool s_just_dropped; // LVGL sends RELEASED, then CLICKED: the drop must not count as a tap

static void end_drag(lv_obj_t *chip) {
    if (!s_dragging) return;
    s_dragging = false;
    s_just_dropped = true;
    lv_obj_t *rail = lv_obj_get_parent(chip);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(chip, 0, 0);
    ui_main_on_chip_reordered(rail);
}

static void drag_to(lv_obj_t *chip, const lv_point_t *p) {
    lv_obj_t *rail = lv_obj_get_parent(chip);
    lv_area_t ra;
    lv_obj_get_coords(rail, &ra);
    if (p->x < ra.x1 + EDGE_SCROLL_ZONE_PX) lv_obj_scroll_by_bounded(rail, EDGE_SCROLL_STEP_PX, 0, LV_ANIM_OFF);
    else if (p->x > ra.x2 - EDGE_SCROLL_ZONE_PX) lv_obj_scroll_by_bounded(rail, -EDGE_SCROLL_STEP_PX, 0, LV_ANIM_OFF);

    uint32_t n = lv_obj_get_child_cnt(rail);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *sib = lv_obj_get_child(rail, i);
        if (sib == chip || !lv_obj_has_flag(sib, UI_CHIP_FLAG)) continue; // skip the "+" button
        lv_area_t a;
        lv_obj_get_coords(sib, &a);
        if (p->x >= a.x1 && p->x <= a.x2) {
            lv_obj_move_to_index(chip, (int32_t)i);
            break;
        }
    }
}

static void chip_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *chip = lv_event_get_target(e);
    uint8_t fx_index = (uint8_t)(uintptr_t)lv_obj_get_user_data(chip);
    const bool movable = !g_effects[fx_index].fixed;
    lv_indev_t *indev = lv_indev_active();

    switch (code) {
    case LV_EVENT_PRESSED:
        s_just_dropped = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!movable) break;
        s_dragging = true;
        // While a chip is held, the rail must not grab the gesture as a scroll.
        lv_obj_remove_flag(lv_obj_get_parent(chip), LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(chip, LV_OPA_70, 0);
        lv_obj_set_style_translate_y(chip, -6, 0);
        break;
    case LV_EVENT_PRESSING:
        if (s_dragging && indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            drag_to(chip, &p);
        }
        break;
    case LV_EVENT_CLICKED:
        if (s_just_dropped) s_just_dropped = false;
        else ui_main_on_chip_tapped(fx_index);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        end_drag(chip);
        break;
    default:
        break;
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
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE | UI_CHIP_FLAG); // CLICKABLE is the lv_obj default in LVGL 9; kept explicit
    lv_obj_set_user_data(chip, (void *)(uintptr_t)fx_index);

    // Top-left tag: a drag-handle glyph for movable chips, "IR" on the pinned CAB.
    lv_obj_t *tag = lv_label_create(chip);
    lv_label_set_text(tag, def->fixed ? "IR" : LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(tag, UI_COLOR_GLYPH, 0);
    lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 9, 6);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, def->label);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *row = lv_obj_create(chip);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 16);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE); // plain lv_obj is clickable by default; don't swallow chip taps

    lv_obj_t *state_text = lv_label_create(row);
    lv_obj_set_style_text_font(state_text, &lv_font_montserrat_12, 0);

    lv_obj_add_event_cb(chip, chip_event_cb, LV_EVENT_ALL, NULL);
    return chip;
}

void ui_effect_chip_refresh(lv_obj_t *chip, bool is_on, bool is_selected) {
    uint8_t fx_index = (uint8_t)(uintptr_t)lv_obj_get_user_data(chip);
    const ui_effect_def_t *def = &g_effects[fx_index];
    lv_obj_t *label = lv_obj_get_child(chip, CHILD_LABEL);
    lv_obj_t *row = lv_obj_get_child(chip, CHILD_ROW);
    lv_obj_t *dot = lv_obj_get_child(row, 0);
    lv_obj_t *state_text = lv_obj_get_child(row, 1);

    lv_obj_set_style_border_color(chip, is_selected ? def->color : UI_COLOR_BORDER, 0);
    lv_obj_set_style_text_color(label, (is_on || is_selected) ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_bg_color(dot, is_on ? def->color : UI_COLOR_OFF, 0);
    lv_label_set_text(state_text, is_on ? "ON" : "BYPASS");
    lv_obj_set_style_text_color(state_text, is_on ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT_MUTED, 0);
}
