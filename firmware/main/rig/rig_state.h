#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "ui_effects_data.h"

// The single source of truth for audio state: effect chain, on/bypass, model per effect, knob
// values, master volume and mute. Every change goes through the setters below, which compare
// old and new, mark what changed and wake the Seed link task. The UI keeps its own view state
// (selected effect, open popups); everything the audio engine needs lives here.
//
// Threading: the setters can be called from any task. Reads through rig() are plain loads of
// small fields and are meant for the LVGL task; other tasks take a consistent copy with
// rig_get_full(). Knob values are stored in g_knob_values (ui_effects_data.c), which the knob
// and slider widgets already read; only rig_state writes it.

typedef enum {
    RIG_SRC_UI,      // touch, in the LVGL task
    RIG_SRC_PRESET,  // a preset was loaded
    RIG_SRC_BOOT,    // the saved rig was restored at boot
    RIG_SRC_KNOB,    // a physical knob
    RIG_SRC_SD,      // the SD model loader
    RIG_SRC_COUNT,
} rig_src_t;

// Dirty bits, read and cleared by the link task with rig_take_dirty().
enum {
    RIG_DIRTY_CHAIN    = 1u << 0,  // processing order changed
    RIG_DIRTY_FX       = 1u << 1,  // some effect's on/bypass or model changed
    RIG_DIRTY_PARAMS   = 1u << 2,  // some knob changed; which ones is in the knob mask
    RIG_DIRTY_MASTER   = 1u << 3,  // master volume or mute changed
    RIG_DIRTY_MUTE_NOW = 1u << 4,  // mute changed: send at once, never wait for coalescing
    RIG_DIRTY_SNAPSHOT = 1u << 5,  // the whole rig was replaced (preset, boot): send a snapshot
};

typedef struct {
    uint8_t chain[UI_CHAIN_MAX];       // effect ids in processing order; CAB is implied last
    uint8_t chain_len;
    bool on[UI_EFFECT_COUNT];          // false = bypassed; CAB is always on
    uint8_t model[UI_EFFECT_COUNT];    // index into g_effects[fx].models, 0 when none
    int32_t master_volume;             // 0..100, kept while muted so un-muting restores it
    bool muted;
    // Non-zero: the amp runs this SD amp profile (CRC32 of its packed weights) instead of the
    // built-in model[FX_AMP]. A hash, not a list index, so it survives SD rescans and reboots.
    uint32_t amp_sd_hash;
} rig_t;

// rig_t plus every knob value: what a snapshot, a preset or the saved rig carries.
typedef struct {
    rig_t r;
    int32_t knobs[UI_EFFECT_COUNT][UI_MAX_KNOBS];
} rig_full_t;

const rig_t *rig(void);
void rig_get_full(rig_full_t *out);

// Each returns true if the value actually changed (and was marked dirty).
bool rig_set_knob(uint8_t fx, uint8_t knob, int32_t value, rig_src_t src);
bool rig_set_fx_on(uint8_t fx, bool on, rig_src_t src);
// values (may be NULL) are the knob values the model brings, applied in the same change.
bool rig_set_model(uint8_t fx, uint8_t model, const int8_t *values, rig_src_t src);
// For the AMP, model is a built-in index (amp_models.h) and selecting one clears amp_sd_hash.
bool rig_set_chain(const uint8_t *chain, uint8_t len, rig_src_t src);
// Selects an SD amp profile by hash (0 = back to the built-in in model[FX_AMP]).
bool rig_set_amp_sd(uint32_t hash, rig_src_t src);
bool rig_set_master_volume(int32_t volume, rig_src_t src);
bool rig_set_muted(bool muted, rig_src_t src);
// Replaces everything (preset load, boot restore) and asks for exactly one snapshot.
void rig_replace(const rig_full_t *full, rig_src_t src);

// For the link task: returns and clears the dirty bits; *knob_mask gets bit (fx * UI_MAX_KNOBS
// + knob) for every knob changed since the last call.
uint32_t rig_take_dirty(uint64_t *knob_mask);
// Called on every change, from the changing task, after the change is committed (the Seed link
// wakes its task with it). Must be quick and must not block. NULL = nobody.
void rig_set_notify(void (*fn)(void));

// Called after a change from a source outside the LVGL task (RIG_SRC_KNOB, RIG_SRC_SD), from
// the caller's task, so the UI can follow. It must not block or call LVGL: record the change
// and let the LVGL task apply it. Touch, preset and boot changes are made by the UI itself.
typedef void (*rig_observer_t)(uint32_t dirty, rig_src_t src);
void rig_set_observer(rig_observer_t cb);

// "amp.gain" style name for a knob (the stable key the wire parameter table uses).
void rig_knob_name(uint8_t fx, uint8_t knob, char *buf, size_t len);

// Starts the change logger (one log line per burst of changes). Call once at boot.
void rig_init(void);
