#pragma once
#include <stdbool.h>
#include <stdint.h>

// The AMP picker's list: the Seed's built-in NAM amps first (AmpId order), then amp profiles
// imported from the SD card. Built-in names come from the Seed's HELLO once it has been heard,
// otherwise from ui_effects_data.c (the same list, compiled in). Readable from any task.

#define AMP_MODELS_MAX_SD 24

typedef struct {
    char name[40];
    char desc[72];
    bool builtin;
    uint8_t builtin_id;     // the Seed's AmpId, for built-ins
    uint32_t sd_hash;       // CRC32 of the packed weights, for SD profiles
    bool rejected;          // an SD file that can't run on the Seed; desc says why (not selectable)
    char file[48];          // SD profiles: the file name on the card
} amp_model_t;

// Allocates the lists and fills in the compiled-in built-ins. Call once, before anything else.
void amp_models_init(void);

int amp_models_count(void);
int amp_models_builtin_count(void);
bool amp_models_get(int index, amp_model_t *out);
// Index of the SD profile with this hash, or -1.
int amp_models_find_sd(uint32_t hash);
// Changes whenever the list or a name changes, so the UI knows to redraw the AMP cards.
uint32_t amp_models_version(void);

// Pulls the built-in names from the Seed link (call periodically from the UI; cheap).
void amp_models_sync_from_seed(void);
// Replaces the SD part of the list (from the SD model scanner).
void amp_models_set_sd(const amp_model_t *list, int count);
