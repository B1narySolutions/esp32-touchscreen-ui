#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "ui_theme.h"

typedef struct {
    const char *key;      // e.g. "gain" - what a real DSP param this maps to
    const char *label;    // display label, e.g. "Gain"
} ui_knob_def_t;

typedef struct {
    const char *id;              // stable identity, e.g. "amp" - used as user_data tag
    const char *label;           // chip label, e.g. "AMP"
    lv_color_t  color;
    const char *model_key;       // NULL if this effect has no model/type picker
    const char **model_options;  // NULL-terminated list, only used when model_key != NULL
    const ui_knob_def_t *knobs;
    uint8_t knob_count;
} ui_effect_def_t;

// Fixed identity index into g_effects[] - NOT the chain position, which changes
// when the user drags a chip. Chain position lives in ui_main's chain_order[].
enum {
    FX_GATE = 0,
    FX_COMP,
    FX_DRIVE,
    FX_AMP,
    FX_CHORUS,
    FX_DELAY,
    FX_REVERB,
};

extern const ui_effect_def_t g_effects[UI_EFFECT_COUNT];

// Default knob values per effect, indexed [fx][knob], mirrors the HTML prototype
extern int32_t g_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS];