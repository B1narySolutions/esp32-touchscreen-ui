#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_theme.h"

// Everything in this file mirrors preview/index.html (EFFECTS, MODEL_OPTIONS, MODEL_VALUES,
// DEFAULT_KNOBS, PRESETS). Keep the two in sync when either changes.

typedef struct {
    const char *key;      // e.g. "gain" - what a real DSP param this maps to
    const char *label;    // display label, e.g. "GAIN"
} ui_knob_def_t;

typedef struct {
    const char *name;
    const char *desc;
    int8_t values[UI_MAX_KNOBS];  // knob values this model loads, in the effect's knob order
} ui_model_def_t;

typedef struct {
    const char *id;              // stable identity, e.g. "amp" - used as user_data tag
    const char *label;           // chip label, e.g. "AMP"
    lv_color_t  color;
    const char *subtitle;        // shown next to the panel title, or NULL
    bool fixed;                  // pinned at the end of the chain, can't be moved or removed (CAB)
    const ui_model_def_t *models;  // NULL if this effect has no model picker
    uint8_t model_count;
    const ui_knob_def_t *knobs;
    uint8_t knob_count;
} ui_effect_def_t;

// Fixed identity index into g_effects[] - NOT the chain position, which changes when the user
// reorders, adds or removes effects. Chain order lives in ui_main. 0..FX_CAB-1 is also the
// order the add/remove catalog lists them in.
enum {
    FX_GATE = 0,
    FX_COMP,
    FX_EQ,
    FX_DRIVE,
    FX_AMP,
    FX_OCTAVE,
    FX_CHORUS,
    FX_PHASER,
    FX_TREMOLO,
    FX_DELAY,
    FX_REVERB,
    FX_CAB,          // always last: pinned after the chain
};
#define UI_CHAIN_MAX FX_CAB  // every effect except CAB can be in the chain

extern const ui_effect_def_t g_effects[UI_EFFECT_COUNT];

// Live knob values per effect, indexed [fx][knob], 0..100.
extern int32_t g_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS];
extern const int8_t g_default_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS];

typedef struct {
    uint8_t fx;
    int8_t values[UI_MAX_KNOBS];
} ui_preset_knobs_t;

typedef struct {
    const char *name;
    const char *tagline;
    uint8_t chain[UI_CHAIN_MAX];
    uint8_t chain_len;
    uint16_t on_mask;                 // bit (1 << fx) set = effect on; CAB is always on
    const ui_preset_knobs_t *knobs;   // overrides on top of g_default_knob_values
    uint8_t knob_count;
    uint8_t models[UI_EFFECT_COUNT];  // model index per effect; 0 = the default model
} ui_preset_t;

#define UI_PRESET_COUNT 5
extern const ui_preset_t g_presets[UI_PRESET_COUNT];

// Approximate cabinet response used to draw the CAB panel's frequency curve. Hand-tuned per IR
// (same numbers as the preview), NOT measured from an impulse response file - replace with an
// FFT of the real IR once IRs are loaded from SD.
typedef struct { float f, g, w; } ui_ir_peak_t;
typedef struct {
    float lo_hz, hi_hz;
    ui_ir_peak_t peaks[3];
    uint8_t peak_count;
} ui_ir_shape_t;
extern const ui_ir_shape_t g_ir_shapes[];  // indexed like g_effects[FX_CAB].models
