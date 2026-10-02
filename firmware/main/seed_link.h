#pragma once
#include <stdbool.h>
#include <stdint.h>

// ESP32-S3 <-> Daisy Seed ("SEED3", running NAM) link over UART, protocol v1
// (docs/SEED_LINK_PROTOCOL.md). Test Mode, the DIAG line and the IN/OUT meters read everything
// through seed_link_get_stats(); audio state reaches the Seed through rig_state, which wakes
// the link task on every change. Nothing here blocks: callers in the LVGL task only copy
// counters or raise flags.
typedef struct {
    bool     connected;          // HELLO received and a valid frame within the last 3 s
    uint32_t packets_tx;
    uint32_t packets_rx;
    uint32_t link_errors;        // frames dropped by the decoder (bad COBS/CRC/length/version)
    float    rtt_ms;             // last PING round trip, 0 until measured
    char     dsp_fw_version[17];
    uint32_t sample_rate_hz;
    uint16_t block_size;
    float    dsp_cpu_pct;        // average DSP load, % of the audio block budget
    char     nam_model[40];
    float    input_peak_dbfs;
    float    output_peak_dbfs;
    uint32_t clip_count;

    // Added with the UART transport
    bool     peer_mock;          // the peer is tools/mock_seed.py: STATUS and METERS are simulated
    bool     meters_valid;       // a METERS frame arrived within the last second
    uint32_t peer_boot_id;
    uint32_t uart_overruns;      // bytes lost in the UART driver before the decoder saw them
    uint32_t snapshots_sent;
    uint16_t snapshot_applied;   // last snapshot the Seed reported as fully applied
    uint32_t dsp_overruns;       // audio callback overruns the Seed reports since its boot
    float    dsp_cpu_peak_pct;
    uint8_t  status_flags;       // SLP_STATUS_* from the last STATUS
    uint8_t  active_kind;        // the model the Seed reports running: SLP_MODEL_*
    uint8_t  active_builtin;     // its AmpId, for built-ins
    uint32_t active_hash;        // its CRC32, for uploaded SD profiles
} seed_link_stats_t;

// Starts the link task. Call once, after rig_state holds the boot state.
void seed_link_init(void);

void seed_link_get_stats(seed_link_stats_t *out);
// Requests a ping; rtt_ms updates when the reply arrives. Returns false if not connected.
bool seed_link_ping(void);
void seed_link_reset_counters(void);
// Human-readable transport description for Test Mode, e.g. "UART0 1000000 baud, GPIO43/44".
const char *seed_link_transport(void);

// The Seed's built-in models, as announced in its HELLO (AmpId order). Returns the count copied.
// The version number changes whenever the list changes, so the UI knows to rebuild.
#define SEED_LINK_MAX_BUILTINS 8
typedef struct {
    uint8_t id;
    char name[25];
} seed_link_builtin_t;
int seed_link_get_builtins(seed_link_builtin_t *out, int max, uint32_t *version);
