#include "ui_effects_data.h"

#define BIT(fx) (1u << (fx))

static const ui_knob_def_t k_gate[]    = { {"thresh","THRESHOLD"}, {"attack","ATTACK"}, {"release","RELEASE"}, {"range","RANGE"} };
static const ui_knob_def_t k_comp[]    = { {"thresh","THRESHOLD"}, {"ratio","RATIO"}, {"attack","ATTACK"}, {"release","RELEASE"}, {"gain","GAIN"} };
static const ui_knob_def_t k_eq[]      = { {"low","LOW"}, {"mid","MID"}, {"high","HIGH"}, {"level","LEVEL"} };
static const ui_knob_def_t k_drive[]   = { {"gain","GAIN"}, {"tone","TONE"}, {"level","LEVEL"} };
static const ui_knob_def_t k_amp[]     = { {"gain","GAIN"}, {"bass","BASS"}, {"mid","MIDDLE"}, {"treble","TREBLE"}, {"level","LEVEL"} };
static const ui_knob_def_t k_octave[]  = { {"mix","MIX"}, {"octUp","OCT UP"}, {"octDown","OCT DOWN"} };
static const ui_knob_def_t k_chorus[]  = { {"rate","RATE"}, {"depth","DEPTH"}, {"mix","MIX"} };
static const ui_knob_def_t k_phaser[]  = { {"rate","RATE"}, {"depth","DEPTH"}, {"mix","MIX"} };
static const ui_knob_def_t k_tremolo[] = { {"rate","RATE"}, {"depth","DEPTH"} };
static const ui_knob_def_t k_delay[]   = { {"time","TIME"}, {"feedback","FEEDBACK"}, {"tone","TONE"}, {"mix","MIX"} };
static const ui_knob_def_t k_reverb[]  = { {"decay","DECAY"}, {"predelay","PRE-DELAY"}, {"tone","TONE"}, {"mix","MIX"} };
static const ui_knob_def_t k_cab[]     = { {"lowcut","LOW CUT"}, {"highcut","HIGH CUT"}, {"level","LEVEL"} };

// The Seed's built-in NAM amps, in its AmpId order (realtime-nam-seed3/models/amps.json); the
// Seed's HELLO supplies the live names when connected. Amp profiles imported from the SD card
// are appended at run time (rig/amp_models.c). The values are the UI knob positions each model
// loads; the amp's tone knobs are not implemented on the Seed yet (docs/SEED_LINK_PROTOCOL.md).
static const ui_model_def_t m_amp[] = {
    { "Fender Twin65",      "Fender Twin Reverb '65 reissue, clean headroom. NAM A2 capture.", { 25, 55, 50, 60, 60 } },
    { "Vox AC30 Chimey",    "Vox AC30 Custom Classic, chimey top-boost breakup. NAM A2 capture.", { 45, 45, 60, 65, 62 } },
    { "Marshall JCM800 G5", "Marshall JCM800 2203, classic rock crunch to lead. NAM A2 capture.", { 62, 55, 58, 68, 66 } },
};
static const ui_model_def_t m_drive[] = {
    { "Tube Screamer",  "Mid-forward overdrive with a smooth, compressed clip.", { 45, 60, 60 } },
    { "Big Muff",       "Thick fuzz-saturated sustain, a wall-of-sound voicing.", { 75, 45, 50 } },
    { "Klon-style",     "Transparent boost that keeps your amp's own tone.",     { 30, 55, 65 } },
    { "Rat Distortion", "Aggressive clipping with a gritty, raw edge.",          { 70, 50, 55 } },
};
static const ui_model_def_t m_chorus[] = {
    { "Classic Chorus", "Lush stereo shimmer, the '80s studio staple.",                 { 35, 50, 40 } },
    { "Dimension",      "Subtle width and depth without an obvious pitch wobble.",      { 20, 30, 35 } },
    { "Ensemble",       "Dense multi-voice modulation for a string-like swirl.",        { 55, 70, 50 } },
};
static const ui_model_def_t m_delay[] = {
    { "Digital Delay", "Clean, precise repeats with no character added.",            { 45, 38, 60, 35 } },
    { "Tape Echo",     "Warm, saturated echoes that degrade slightly each repeat.",  { 55, 50, 40, 38 } },
    { "Ping-Pong",     "Repeats alternate left/right for a wide stereo bounce.",     { 40, 45, 55, 40 } },
};
static const ui_model_def_t m_reverb[] = {
    { "Hall",   "Large, spacious decay for orchestral-scale ambience.",       { 65, 25, 55, 35 } },
    { "Room",   "Tight, natural early reflections for a close, live feel.",  { 25, 10, 50, 28 } },
    { "Plate",  "Bright, dense metallic shimmer, a classic studio reverb.",  { 50, 15, 65, 32 } },
    { "Spring", "Splashy, boingy character straight out of an amp tank.",   { 35,  5, 45, 30 } },
};
static const ui_model_def_t m_tremolo[] = {
    { "Sine",     "Smooth, rounded volume pulsing.",                { 40, 45 } },
    { "Square",   "Hard on/off chop for a staggered, choppy feel.", { 55, 70 } },
    { "Triangle", "Linear ramp between loud and soft.",             { 45, 55 } },
};
static const ui_model_def_t m_cab[] = {
    { "1x12 Open Back",   "Airy, focused combo cab with a scooped low end.",      { 20, 70, 55 } },
    { "2x12 Blue Alnico", "Sparkly, chimey voice with smooth, open highs.",       { 18, 78, 55 } },
    { "4x12 Greenback",   "Warm, mid-forward classic rock cabinet.",              { 25, 60, 58 } },
    { "4x12 V30 Modern",  "Tight, aggressive top end built for high gain.",       { 35, 58, 60 } },
    { "User IR 01",       "Custom impulse response loaded from SD card.",         { 20, 70, 55 } },
};

#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
#define FX(id_, label_, rgb_, sub_, fixed_, models_, knobs_) \
    { id_, label_, LV_COLOR_MAKE(((rgb_) >> 16) & 0xff, ((rgb_) >> 8) & 0xff, (rgb_) & 0xff), \
      sub_, fixed_, models_, N_##models_, knobs_, N(knobs_) }

// model counts, spelled out so the FX() macro can pick them up
#define N_NULL 0
#define N_m_amp     N(m_amp)
#define N_m_drive   N(m_drive)
#define N_m_chorus  N(m_chorus)
#define N_m_delay   N(m_delay)
#define N_m_reverb  N(m_reverb)
#define N_m_tremolo N(m_tremolo)
#define N_m_cab     N(m_cab)

const ui_effect_def_t g_effects[UI_EFFECT_COUNT] = {
    [FX_GATE]    = FX("gate",    "GATE",    0x5ecdbb, NULL, false, NULL,      k_gate),
    [FX_COMP]    = FX("comp",    "COMP",    0xd9b26a, NULL, false, NULL,      k_comp),
    [FX_EQ]      = FX("eq",      "EQ",      0x7fd9a8, NULL, false, NULL,      k_eq),
    [FX_DRIVE]   = FX("drive",   "DRIVE",   0xe2795c, NULL, false, m_drive,   k_drive),
    [FX_AMP]     = FX("amp",     "AMP",     0xe9c98a, NULL, false, m_amp,     k_amp),
    [FX_OCTAVE]  = FX("octave",  "OCTAVE",  0xe05c8a, NULL, false, NULL,      k_octave),
    [FX_CHORUS]  = FX("chorus",  "CHORUS",  0x7fa3e0, NULL, false, m_chorus,  k_chorus),
    [FX_PHASER]  = FX("phaser",  "PHASER",  0x5cc9e0, NULL, false, NULL,      k_phaser),
    [FX_TREMOLO] = FX("tremolo", "TREMOLO", 0xd9c95c, NULL, false, m_tremolo, k_tremolo),
    [FX_DELAY]   = FX("delay",   "DELAY",   0x8b6fd8, NULL, false, m_delay,   k_delay),
    [FX_REVERB]  = FX("reverb",  "REVERB",  0xb07fe0, NULL, false, m_reverb,  k_reverb),
    [FX_CAB]     = FX("cab",     "CAB",     0x8fb0c4, "IMPULSE RESPONSE", true, m_cab, k_cab),
};

const int8_t g_default_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS] = {
    [FX_GATE]    = { 35, 20, 40, 70 },
    [FX_COMP]    = { 45, 55, 25, 50, 50 },
    [FX_EQ]      = { 50, 50, 50, 60 },
    [FX_DRIVE]   = { 40, 60, 55 },
    [FX_AMP]     = { 32, 50, 45, 71, 65 },
    [FX_OCTAVE]  = { 40, 30, 20 },
    [FX_CHORUS]  = { 35, 50, 40 },
    [FX_PHASER]  = { 35, 45, 35 },
    [FX_TREMOLO] = { 40, 45 },
    [FX_DELAY]   = { 45, 38, 55, 35 },
    [FX_REVERB]  = { 55, 20, 60, 42 },
    [FX_CAB]     = { 20, 70, 55 },
};

int32_t g_knob_values[UI_EFFECT_COUNT][UI_MAX_KNOBS] = {
    [FX_GATE]    = { 35, 20, 40, 70 },
    [FX_COMP]    = { 45, 55, 25, 50, 50 },
    [FX_EQ]      = { 50, 50, 50, 60 },
    [FX_DRIVE]   = { 40, 60, 55 },
    [FX_AMP]     = { 32, 50, 45, 71, 65 },
    [FX_OCTAVE]  = { 40, 30, 20 },
    [FX_CHORUS]  = { 35, 50, 40 },
    [FX_PHASER]  = { 35, 45, 35 },
    [FX_TREMOLO] = { 40, 45 },
    [FX_DELAY]   = { 45, 38, 55, 35 },
    [FX_REVERB]  = { 55, 20, 60, 42 },
    [FX_CAB]     = { 20, 70, 55 },
};

static const ui_preset_knobs_t p_clean[] = {
    { FX_GATE,   { 20, 10, 30, 50 } },
    { FX_COMP,   { 40, 40, 20, 45, 45 } },
    { FX_AMP,    { 25, 55, 50, 60, 60 } },
    { FX_CHORUS, { 25, 35, 25 } },
    { FX_REVERB, { 40, 15, 55, 25 } },
    { FX_CAB,    { 20, 70, 55 } },
};
static const ui_preset_knobs_t p_crunch[] = {
    { FX_DRIVE,  { 55, 58, 60 } },
    { FX_AMP,    { 58, 52, 55, 66, 68 } },
    { FX_REVERB, { 30, 10, 50, 18 } },
    { FX_CAB,    { 25, 65, 58 } },
};
static const ui_preset_knobs_t p_lead[] = {
    { FX_COMP,   { 50, 60, 15, 40, 55 } },
    { FX_DRIVE,  { 70, 65, 70 } },
    { FX_AMP,    { 75, 48, 60, 68, 72 } },
    { FX_DELAY,  { 35, 30, 50, 22 } },
    { FX_REVERB, { 45, 15, 55, 20 } },
    { FX_CAB,    { 30, 60, 60 } },
};
static const ui_preset_knobs_t p_ambient[] = {
    { FX_COMP,   { 42, 35, 25, 50, 40 } },
    { FX_AMP,    { 18, 50, 40, 55, 55 } },
    { FX_CHORUS, { 45, 70, 55 } },
    { FX_DELAY,  { 65, 60, 45, 50 } },
    { FX_REVERB, { 80, 30, 50, 60 } },
    { FX_CAB,    { 15, 85, 52 } },
};
static const ui_preset_knobs_t p_metal[] = {
    { FX_GATE,  { 55, 5, 20, 85 } },
    { FX_COMP,  { 35, 30, 10, 25, 40 } },
    { FX_DRIVE, { 85, 50, 65 } },
    { FX_AMP,   { 88, 45, 35, 70, 75 } },
    { FX_EQ,    { 40, 35, 55, 58 } },
    { FX_CAB,   { 45, 55, 62 } },
};

const ui_preset_t g_presets[UI_PRESET_COUNT] = {
    { "Clean Rhythm", "Bright clean tone for verses",
      { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_CHORUS, FX_DELAY, FX_REVERB }, 7,
      BIT(FX_GATE) | BIT(FX_COMP) | BIT(FX_AMP) | BIT(FX_CHORUS) | BIT(FX_REVERB),
      p_clean, N(p_clean),
      { [FX_AMP] = 0, [FX_CAB] = 0 } },
    { "Crunch Rock", "Driven rhythm with amp-like grit",
      { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_CHORUS, FX_DELAY, FX_REVERB }, 7,
      BIT(FX_GATE) | BIT(FX_DRIVE) | BIT(FX_AMP) | BIT(FX_REVERB),
      p_crunch, N(p_crunch),
      { [FX_DRIVE] = 0, [FX_AMP] = 2, [FX_CAB] = 2 } },
    { "Lead Boost", "Singing sustain for solos",
      { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_DELAY, FX_CHORUS, FX_REVERB }, 7,
      BIT(FX_GATE) | BIT(FX_COMP) | BIT(FX_DRIVE) | BIT(FX_AMP) | BIT(FX_DELAY) | BIT(FX_REVERB),
      p_lead, N(p_lead),
      { [FX_DRIVE] = 2, [FX_AMP] = 2, [FX_DELAY] = 0, [FX_REVERB] = 0, [FX_CAB] = 3 } },
    { "Ambient Wash", "Spacious pads and long tails",
      { FX_GATE, FX_COMP, FX_AMP, FX_CHORUS, FX_DRIVE, FX_DELAY, FX_REVERB }, 7,
      BIT(FX_COMP) | BIT(FX_AMP) | BIT(FX_CHORUS) | BIT(FX_DELAY) | BIT(FX_REVERB),
      p_ambient, N(p_ambient),
      { [FX_AMP] = 1, [FX_CHORUS] = 2, [FX_DELAY] = 1, [FX_REVERB] = 0, [FX_CAB] = 1 } },
    { "Metal Tight", "Palm-muted precision, tight low end",
      { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_EQ, FX_CHORUS, FX_DELAY, FX_REVERB }, 8,
      BIT(FX_GATE) | BIT(FX_COMP) | BIT(FX_DRIVE) | BIT(FX_AMP) | BIT(FX_EQ),
      p_metal, N(p_metal),
      { [FX_DRIVE] = 3, [FX_AMP] = 2, [FX_CAB] = 3 } },
};

const ui_ir_shape_t g_ir_shapes[] = {
    { 95, 5800, { { 2200, 3, .6f }, { 400, -2, .8f } }, 2 },
    { 85, 7200, { { 3500, 4, .5f }, { 180, 1.5f, .6f } }, 2 },
    { 75, 5200, { { 2200, 5, .4f }, { 120, 2, .5f }, { 4200, -3, .4f } }, 3 },
    { 80, 6500, { { 3200, 6, .35f }, { 1200, -2, .6f }, { 120, 1, .6f } }, 3 },
    { 70, 8000, { { 2800, 3, .7f }, { 600, -1.5f, .7f } }, 2 },
};
