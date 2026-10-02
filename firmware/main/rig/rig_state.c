#include "rig_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "amp_models.h"

static const char *TAG = "rig";

// Defaults match the UI before any preset or saved rig is loaded.
static rig_t s_rig = {
    .chain = { FX_GATE, FX_COMP, FX_DRIVE, FX_AMP, FX_CHORUS, FX_DELAY, FX_REVERB },
    .chain_len = 7,
    .on = { [FX_AMP] = true, [FX_CAB] = true },
    .master_volume = 63,
};

// Guards s_rig, g_knob_values writes and the dirty state. Held only for a few loads and stores.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dirty;
static uint64_t s_knob_dirty;
static void (*s_notify)(void);
static rig_observer_t s_observer;

// Burst logger: every change restarts a short timer; when changes stop, one line describes
// the difference from the last logged state.
#define LOG_QUIET_US (250 * 1000)
static esp_timer_handle_t s_log_timer;
static rig_full_t s_logged;
static uint32_t s_burst_changes;
static rig_src_t s_burst_src;

static const char *k_src_names[RIG_SRC_COUNT] = { "touch", "preset", "boot", "knob", "sd" };

const rig_t *rig(void) { return &s_rig; }

void rig_get_full(rig_full_t *out) {
    taskENTER_CRITICAL(&s_mux);
    out->r = s_rig;
    memcpy(out->knobs, g_knob_values, sizeof(out->knobs));
    taskEXIT_CRITICAL(&s_mux);
}

void rig_knob_name(uint8_t fx, uint8_t knob, char *buf, size_t len) {
    if (fx < UI_EFFECT_COUNT && knob < g_effects[fx].knob_count) {
        snprintf(buf, len, "%s.%s", g_effects[fx].id, g_effects[fx].knobs[knob].key);
    } else {
        snprintf(buf, len, "fx%u.k%u", fx, knob);
    }
}

// Runs after a change has been committed (outside the critical section).
static void changed(uint32_t dirty, rig_src_t src) {
    taskENTER_CRITICAL(&s_mux);
    s_burst_changes++;
    s_burst_src = src;
    taskEXIT_CRITICAL(&s_mux);

    if (s_notify) s_notify();
    if ((src == RIG_SRC_KNOB || src == RIG_SRC_SD) && s_observer) s_observer(dirty, src);

    if (s_log_timer) {
        // restart() fails when the timer isn't running, start_once() when it is; one succeeds.
        if (esp_timer_restart(s_log_timer, LOG_QUIET_US) != ESP_OK) esp_timer_start_once(s_log_timer, LOG_QUIET_US);
    }
}

static int32_t clamp_pct(int32_t v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

bool rig_set_knob(uint8_t fx, uint8_t knob, int32_t value, rig_src_t src) {
    if (fx >= UI_EFFECT_COUNT || knob >= g_effects[fx].knob_count) return false;
    value = clamp_pct(value);
    taskENTER_CRITICAL(&s_mux);
    const bool diff = g_knob_values[fx][knob] != value;
    if (diff) {
        g_knob_values[fx][knob] = value;
        s_dirty |= RIG_DIRTY_PARAMS;
        s_knob_dirty |= 1ull << (fx * UI_MAX_KNOBS + knob);
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_PARAMS, src);
    return diff;
}

bool rig_set_fx_on(uint8_t fx, bool on, rig_src_t src) {
    if (fx >= UI_EFFECT_COUNT) return false;
    if (fx == FX_CAB) on = true; // the cabinet is never bypassed
    taskENTER_CRITICAL(&s_mux);
    const bool diff = s_rig.on[fx] != on;
    if (diff) {
        s_rig.on[fx] = on;
        s_dirty |= RIG_DIRTY_FX;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_FX, src);
    return diff;
}

static int model_count(uint8_t fx) { return fx == FX_AMP ? amp_models_builtin_count() : g_effects[fx].model_count; }

bool rig_set_model(uint8_t fx, uint8_t model, const int8_t *values, rig_src_t src) {
    if (fx >= UI_EFFECT_COUNT || model >= model_count(fx)) return false;
    uint32_t dirty = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_rig.model[fx] != model || (fx == FX_AMP && s_rig.amp_sd_hash)) {
        s_rig.model[fx] = model;
        if (fx == FX_AMP) s_rig.amp_sd_hash = 0;
        dirty |= RIG_DIRTY_FX;
    }
    for (int k = 0; values && k < g_effects[fx].knob_count; k++) {
        const int32_t v = clamp_pct(values[k]);
        if (g_knob_values[fx][k] != v) {
            g_knob_values[fx][k] = v;
            s_knob_dirty |= 1ull << (fx * UI_MAX_KNOBS + k);
            dirty |= RIG_DIRTY_PARAMS;
        }
    }
    s_dirty |= dirty;
    taskEXIT_CRITICAL(&s_mux);
    if (dirty) changed(dirty, src);
    return dirty != 0;
}

bool rig_set_chain(const uint8_t *chain, uint8_t len, rig_src_t src) {
    if (len == 0 || len > UI_CHAIN_MAX) return false;
    uint32_t seen = 0;
    for (int i = 0; i < len; i++) {
        if (chain[i] >= FX_CAB || (seen & (1u << chain[i]))) return false; // CAB is implied last
        seen |= 1u << chain[i];
    }
    taskENTER_CRITICAL(&s_mux);
    const bool diff = len != s_rig.chain_len || memcmp(chain, s_rig.chain, len) != 0;
    if (diff) {
        memset(s_rig.chain, 0, sizeof(s_rig.chain));
        memcpy(s_rig.chain, chain, len);
        s_rig.chain_len = len;
        s_dirty |= RIG_DIRTY_CHAIN;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_CHAIN, src);
    return diff;
}

bool rig_set_amp_sd(uint32_t hash, rig_src_t src) {
    taskENTER_CRITICAL(&s_mux);
    const bool diff = s_rig.amp_sd_hash != hash;
    if (diff) {
        s_rig.amp_sd_hash = hash;
        s_dirty |= RIG_DIRTY_FX;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_FX, src);
    return diff;
}

bool rig_set_master_volume(int32_t volume, rig_src_t src) {
    volume = clamp_pct(volume);
    taskENTER_CRITICAL(&s_mux);
    const bool diff = s_rig.master_volume != volume;
    if (diff) {
        s_rig.master_volume = volume;
        s_dirty |= RIG_DIRTY_MASTER;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_MASTER, src);
    return diff;
}

bool rig_set_muted(bool muted, rig_src_t src) {
    taskENTER_CRITICAL(&s_mux);
    const bool diff = s_rig.muted != muted;
    if (diff) {
        s_rig.muted = muted;
        s_dirty |= RIG_DIRTY_MASTER | RIG_DIRTY_MUTE_NOW;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (diff) changed(RIG_DIRTY_MASTER | RIG_DIRTY_MUTE_NOW, src);
    return diff;
}

void rig_replace(const rig_full_t *full, rig_src_t src) {
    taskENTER_CRITICAL(&s_mux);
    s_rig = full->r;
    s_rig.on[FX_CAB] = true;
    memcpy(g_knob_values, full->knobs, sizeof(full->knobs));
    s_dirty |= RIG_DIRTY_SNAPSHOT;
    s_knob_dirty = 0; // the snapshot carries every knob
    taskEXIT_CRITICAL(&s_mux);
    changed(RIG_DIRTY_SNAPSHOT, src);
}

uint32_t rig_take_dirty(uint64_t *knob_mask) {
    taskENTER_CRITICAL(&s_mux);
    uint32_t d = s_dirty;
    *knob_mask = s_knob_dirty;
    s_dirty = 0;
    s_knob_dirty = 0;
    taskEXIT_CRITICAL(&s_mux);
    return d;
}

void rig_set_notify(void (*fn)(void)) { s_notify = fn; }
void rig_set_observer(rig_observer_t cb) { s_observer = cb; }

// ---------------------------------------------------------------------------------------------
// Burst logger

typedef struct {
    char buf[384];
    size_t n;
} line_t;

static void add(line_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(line_t *l, const char *fmt, ...) {
    if (l->n >= sizeof(l->buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(l->buf + l->n, sizeof(l->buf) - l->n, fmt, ap);
    va_end(ap);
    if (w > 0) l->n += (size_t)w;
    if (l->n >= sizeof(l->buf)) l->n = sizeof(l->buf) - 1; // truncated
}

static void log_burst(void *arg) {
    (void)arg;
    rig_full_t cur;
    rig_get_full(&cur);
    taskENTER_CRITICAL(&s_mux);
    const uint32_t count = s_burst_changes;
    const rig_src_t src = s_burst_src;
    s_burst_changes = 0;
    taskEXIT_CRITICAL(&s_mux);

    const rig_t *a = &s_logged.r, *b = &cur.r;
    line_t l = { .n = 0 };
    add(&l, "%s x%lu:", k_src_names[src], (unsigned long)count);
    const size_t head = l.n;

    if (a->chain_len != b->chain_len || memcmp(a->chain, b->chain, sizeof(a->chain)) != 0) {
        add(&l, " chain");
        for (int i = 0; i < b->chain_len; i++) add(&l, "%c%s", i ? '>' : '=', g_effects[b->chain[i]].id);
        add(&l, ";");
    }
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        if (a->on[fx] != b->on[fx]) add(&l, " %s %s;", g_effects[fx].id, b->on[fx] ? "on" : "bypass");
        if (fx == FX_AMP && (a->model[fx] != b->model[fx] || a->amp_sd_hash != b->amp_sd_hash)) {
            amp_model_t m;
            const int idx = b->amp_sd_hash ? amp_models_find_sd(b->amp_sd_hash) : b->model[fx];
            if (amp_models_get(idx, &m)) add(&l, " amp model=%s;", m.name);
            else add(&l, " amp model=SD %08lx (not on the card);", (unsigned long)b->amp_sd_hash);
        } else if (a->model[fx] != b->model[fx]) {
            add(&l, " %s model=%s;", g_effects[fx].id, g_effects[fx].models ? g_effects[fx].models[b->model[fx]].name : "?");
        }
        for (int k = 0; k < g_effects[fx].knob_count; k++) {
            if (s_logged.knobs[fx][k] != cur.knobs[fx][k]) {
                char name[32];
                rig_knob_name(fx, k, name, sizeof(name));
                add(&l, " %s %ld->%ld;", name, (long)s_logged.knobs[fx][k], (long)cur.knobs[fx][k]);
            }
        }
    }
    if (a->master_volume != b->master_volume) add(&l, " master %ld->%ld;", (long)a->master_volume, (long)b->master_volume);
    if (a->muted != b->muted) add(&l, " %s;", b->muted ? "MUTED" : "unmuted");
    if (l.n == head) add(&l, " no net change");

    ESP_LOGI(TAG, "%s", l.buf);
    s_logged = cur;
}

void rig_init(void) {
    rig_get_full(&s_logged);
    const esp_timer_create_args_t args = { .callback = log_burst, .name = "rig_log" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_log_timer));
}
