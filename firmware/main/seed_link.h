#pragma once
#include <stdbool.h>
#include <stdint.h>

// ESP32-S3 <-> Daisy Seed ("SEED3", running NAM) link. Transport and wire format are still
// undecided (I2C was the first plan; as of 2026-10-01 the team is leaning towards UART, with
// I2C going to the potentiometers), so this is only the seam: Test Mode and the rest of the
// UI read everything through seed_link_get_stats(), and the real transport fills it in.
typedef struct {
    bool     connected;
    uint32_t packets_tx;
    uint32_t packets_rx;
    uint32_t link_errors;
    float    rtt_ms;
    char     dsp_fw_version[16];
    uint32_t sample_rate_hz;
    uint16_t block_size;
    float    dsp_cpu_pct;
    char     nam_model[40];
    float    input_peak_dbfs;
    float    output_peak_dbfs;
    uint32_t clip_count;
} seed_link_stats_t;

void seed_link_get_stats(seed_link_stats_t *out);
// Sends a ping and updates rtt_ms when the reply arrives. Returns false if not connected.
bool seed_link_ping(void);
void seed_link_reset_counters(void);
