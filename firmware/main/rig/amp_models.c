#include "amp_models.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "seed_link.h"
#include "ui_effects_data.h"

static const char *TAG = "amp_models";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Both lists live in PSRAM (about 4 KB; internal RAM is reserved for what must be there).
static amp_model_t *s_builtin;            // SEED_LINK_MAX_BUILTINS entries
static int s_builtin_count = -1;          // -1 until amp_models_init() fills in the compiled-in list
static amp_model_t *s_sd;                 // AMP_MODELS_MAX_SD entries
static int s_sd_count;
static uint32_t s_version = 1;
static uint32_t s_seed_version;           // seed_link's builtins version we last copied

static int count_unlocked(void) { return s_builtin_count < 0 ? 0 : s_builtin_count; }

// Compiled-in built-ins (ui_effects_data.c mirrors the Seed's amps.json order).
void amp_models_init(void) {
    if (s_builtin) return;
    s_builtin = heap_caps_calloc(SEED_LINK_MAX_BUILTINS, sizeof(amp_model_t), MALLOC_CAP_SPIRAM);
    s_sd = heap_caps_calloc(AMP_MODELS_MAX_SD, sizeof(amp_model_t), MALLOC_CAP_SPIRAM);
    assert(s_builtin && s_sd);
    const ui_effect_def_t *amp = &g_effects[FX_AMP];
    for (int i = 0; i < amp->model_count && i < SEED_LINK_MAX_BUILTINS; i++) {
        amp_model_t *m = &s_builtin[i];
        memset(m, 0, sizeof(*m));
        snprintf(m->name, sizeof(m->name), "%s", amp->models[i].name);
        snprintf(m->desc, sizeof(m->desc), "%s", amp->models[i].desc);
        m->builtin = true;
        m->builtin_id = (uint8_t)(i + 1);
    }
    portENTER_CRITICAL(&s_mux);
    s_builtin_count = amp->model_count;
    portEXIT_CRITICAL(&s_mux);
}

int amp_models_builtin_count(void) {
    portENTER_CRITICAL(&s_mux);
    int n = count_unlocked();
    portEXIT_CRITICAL(&s_mux);
    return n;
}

int amp_models_count(void) {
    portENTER_CRITICAL(&s_mux);
    int n = count_unlocked() + s_sd_count;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

bool amp_models_get(int index, amp_model_t *out) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (index >= 0 && index < count_unlocked()) {
        *out = s_builtin[index];
        ok = true;
    } else if (index >= count_unlocked() && index < count_unlocked() + s_sd_count) {
        *out = s_sd[index - count_unlocked()];
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

int amp_models_find_sd(uint32_t hash) {
    int found = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_sd_count && found < 0; i++) {
        if (s_sd[i].sd_hash == hash) found = count_unlocked() + i;
    }
    portEXIT_CRITICAL(&s_mux);
    return found;
}

uint32_t amp_models_version(void) {
    portENTER_CRITICAL(&s_mux);
    uint32_t v = s_version;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void amp_models_sync_from_seed(void) {
    seed_link_builtin_t list[SEED_LINK_MAX_BUILTINS];
    uint32_t version;
    const int n = seed_link_get_builtins(list, SEED_LINK_MAX_BUILTINS, &version);
    if (version == s_seed_version || n == 0 || !s_builtin) return;
    s_seed_version = version;

    // The Seed is the authority on what it can run. Keep the compiled-in description when the
    // Seed's id matches one we know; the Seed only sends names.
    portENTER_CRITICAL(&s_mux);
    amp_model_t next[SEED_LINK_MAX_BUILTINS];
    for (int i = 0; i < n; i++) {
        memset(&next[i], 0, sizeof(next[i]));
        snprintf(next[i].name, sizeof(next[i].name), "%s", list[i].name);
        next[i].builtin = true;
        next[i].builtin_id = list[i].id;
        for (int k = 0; k < s_builtin_count; k++) {
            if (s_builtin[k].builtin_id == list[i].id) memcpy(next[i].desc, s_builtin[k].desc, sizeof(next[i].desc));
        }
        if (!next[i].desc[0]) snprintf(next[i].desc, sizeof(next[i].desc), "Built into the Seed (AmpId %u).", list[i].id);
    }
    const bool count_changed = n != s_builtin_count;
    memcpy(s_builtin, next, sizeof(next[0]) * (size_t)n);
    s_builtin_count = n;
    s_version++;
    portEXIT_CRITICAL(&s_mux);
    if (count_changed) {
        ESP_LOGW(TAG, "the Seed reports %d built-in amps; the UI was built for %d", n, g_effects[FX_AMP].model_count);
    }
}

void amp_models_set_sd(const amp_model_t *list, int count) {
    if (!s_sd) return;
    if (count > AMP_MODELS_MAX_SD) count = AMP_MODELS_MAX_SD;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_sd, list, sizeof(list[0]) * (size_t)count);
    s_sd_count = count;
    s_version++;
    portEXIT_CRITICAL(&s_mux);
}
