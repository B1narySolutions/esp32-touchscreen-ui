#include "ui_effects_data.h"

static const ui_knob_def_t k_gate[]   = { {"thresh","Threshold"}, {"attack","Attack"}, {"release","Release"}, {"range","Range"} };
static const ui_knob_def_t k_comp[]   = { {"thresh","Threshold"}, {"ratio","Ratio"}, {"attack","Attack"}, {"release","Release"}, {"gain","Gain"} };
static const ui_knob_def_t k_drive[]  = { {"gain","Gain"}, {"tone","Tone"}, {"level","Level"} };
static const ui_knob_def_t k_amp[]    = { {"gain","Gain"}, {"bass","Bass"}, {"mid","Middle"}, {"treble","Treble"}, {"level","Level"} };
static const ui_knob_def_t k_chorus[] = { {"rate","Rate"}, {"depth","Depth"}, {"mix","Mix"} };
static const ui_knob_def_t k_delay[]  = { {"time","Time"}, {"feedback","Feedback"}, {"tone","Tone"}, {"mix","Mix"} };
static const ui_knob_def_t k_reverb[] = { {"decay","Decay"}, {"predelay","Pre-Delay"}, {"tone","Tone"}, {"mix","Mix"} };

static const char *models_amp[]    = { "Fender '65 Twin Reverb", "Vox AC30", "Marshall Plexi", "Mesa Rectifier", NULL };
static const char *models_drive[]  = { "Tube Screamer", "Big Muff", "Klon-style", "Rat Distortion", NULL };
static const char *models_chorus[] = { "Classic Chorus", "Dimension", "Ensemble", NULL };
static const char *models_delay[]  = { "Digital Delay", "Tape Echo", "Ping-Pong", NULL };
static const char *models_reverb[] = { "Hall", "Room", "Plate", "Spring", NULL };

const ui_effect_def_t g_effects[UI_EFFECT_COUNT] = {
    [FX_GATE]   = { "gate",   "GATE",   LV_COLOR_MAKE(0x5e, 0xcd, 0xbb), NULL,     NULL,          k_gate,   4 },
    [FX_COMP]   = { "comp",   "COMP",   LV_COLOR_MAKE(0xd9, 0xb2, 0x6a), NULL,     NULL,          k_comp,   5 },
    [FX_DRIVE]  = { "drive",  "DRIVE",  LV_COLOR_MAKE(0xe2, 0x79, 0x5c), "drive",  models_drive,  k_drive,  3 },
    [FX_AMP]    = { "amp",    "AMP",    LV_COLOR_MAKE(0xe9, 0xc9, 0x8a), "amp",    models_amp,    k_amp,    5 },
    [FX_CHORUS] = { "chorus", "CHORUS", LV_COLOR_MAKE(0x7f, 0xa3, 0xe0), "chorus", models_chorus, k_chorus, 3 },
    [FX_DELAY]  = { "delay",  "DELAY",  LV_COLOR_MAKE(0x8b, 0x6f, 0xd8), "delay",  models_delay,  k_delay,  4 },
    [FX_REVERB] = { "reverb", "REVERB", LV_COLOR_MAKE(0xb0, 0x7f, 0xe0), "reverb", models_reverb, k_reverb, 4 },
};

int32_t g_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS] = {
    [FX_GATE]   = { 35, 20, 40, 70, 0 },
    [FX_COMP]   = { 45, 55, 25, 50, 50 },
    [FX_DRIVE]  = { 40, 60, 55, 0, 0 },
    [FX_AMP]    = { 32, 50, 45, 71, 65 },
    [FX_CHORUS] = { 35, 50, 40, 0, 0 },
    [FX_DELAY]  = { 45, 38, 55, 35, 0 },
    [FX_REVERB] = { 55, 20, 60, 42, 0 },
};
