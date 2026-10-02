#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TONE3000 A2 amp profiles on the microSD card, in /sdcard/nam/*.nam.
//
// A worker task mounts the card and converts every .nam file with nam_a2 (validation and
// packing exactly as convert_a2.py), caching the packed weights as
// /sdcard/nam/.cache/<sha256 of the file>.a2l so later boots skip the JSON. Accepted profiles
// join the AMP picker (amp_models.h) after the Seed's built-ins; rejected files appear there too,
// greyed out, with the reason. Packed weights stay in PSRAM so an upload never waits for the card.

void nam_store_start(void);
// Scans the card again (after a card swap). Non-blocking; safe from the LVGL task.
void nam_store_rescan(void);

// The packed weights (NAM_A2_WEIGHT_COUNT floats) of the profile with this CRC32, or NULL.
// The pointer stays valid until the next rescan. name (may be NULL) gets the display name.
const float *nam_store_weights(uint32_t hash, char *name, size_t name_len);

typedef struct {
    bool scanning;
    bool card_ok;
    int accepted, rejected;
    char status[96];     // one line for the UI, e.g. "2 amp profiles from the SD card"
} nam_store_status_t;
void nam_store_status(nam_store_status_t *out);
