/*
 * TONE3000 A2 .nam files -> the Seed's packed A2-Lite weights. Pure C99, no allocation, no
 * ESP-IDF, so the host tests run the exact code the ESP32 runs.
 *
 * A port of realtime-nam-seed3/scripts/convert_a2.py (the authority): extract() for an A2
 * SlimmableContainer (the max_value == 0.5 submodel), validate(), to_float32(),
 * check_head_scale() and pack(). Accept/reject decisions follow Python's semantics, including
 * its equality rules (1 == 1.0 == true, dict key order irrelevant, last duplicate key wins).
 * Weights are parsed to double then rounded to float32, as Python does; the golden test in
 * tests/host/test_nam.c checks the bytes against convert_a2.py's own output.
 *
 * The JSON is walked in place (no tree is built): a TONE3000 A2 container is ~300 KB with
 * ~14,000 numbers, and a DOM would cost megabytes.
 */
#ifndef NAM_A2_H
#define NAM_A2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nam_a2_expected.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAM_OK = 0,
    NAM_ERR_JSON,          // not valid JSON
    NAM_ERR_NOT_MODEL,     // JSON, but not a NAM model object
    NAM_ERR_CONTAINER,     // A2 container without exactly one Lite (0.5) submodel
    NAM_ERR_ARCH,          // not a WaveNet (e.g. LSTM)
    NAM_ERR_VERSION,       // not NAM 0.7.0 (e.g. an A1 / standard model)
    NAM_ERR_SAMPLE_RATE,   // not 48 kHz
    NAM_ERR_CONFIG,        // not the A2-Lite layer layout
    NAM_ERR_WEIGHT_COUNT,
    NAM_ERR_WEIGHT_VALUE,  // a weight that isn't a finite float32
    NAM_ERR_HEAD_SCALE,    // config.head_scale disagrees with the weight stream
    NAM_ERR_TOO_DEEP,      // nesting deeper than the parser allows
} nam_err_t;

typedef struct {
    float weights[NAM_A2_WEIGHT_COUNT];  // packed in the engine's order, little-endian on the wire
    uint32_t crc32;                      // CRC-32 (zlib) of the packed bytes
    char name[64];                       // metadata.name, or "" if the file has none
} nam_a2_t;

// Parses, validates and packs one .nam file. json must be NUL-terminated at json[len] (the
// parser relies on it as a sentinel). On failure, reason gets a short message for the UI.
nam_err_t nam_a2_load(const char *json, size_t len, nam_a2_t *out, char *reason, size_t reason_len);

#ifdef __cplusplus
}
#endif
#endif
