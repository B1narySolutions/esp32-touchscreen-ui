/*
 * slp_receiver: the Seed's side of the link protocol, ready to drop into the Seed firmware.
 * Pure C99 on top of seed_link_proto.c, no allocation, no libDaisy dependency; the Seed only
 * supplies a UART write function, a millisecond clock and the hooks below. The ESP32 firmware
 * doesn't compile this file; tests/host/test_receiver.c runs it against the real encoder.
 *
 * Threading: call everything from the Seed's main loop. Never from the audio callback, and not
 * from the UART DMA callback either: that callback should only copy bytes into a ring buffer
 * that the main loop hands to slp_rx_feed(). See docs/SEED_INTEGRATION_GUIDE.md.
 */
#ifndef SLP_RECEIVER_H
#define SLP_RECEIVER_H

#include "seed_link_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Required.
    void (*send)(void *ctx, const uint8_t *bytes, size_t len); // write to the UART (may block briefly)
    uint32_t (*now_ms)(void *ctx);                              // monotonic milliseconds
    void *ctx;                                                  // passed back to every callback

    // Identity, sent in HELLO.
    uint32_t boot_id;               // random per boot (e.g. from the RNG or a timer at startup)
    const char *fw_version;
    uint32_t sample_rate_hz;        // 48000
    uint16_t block_size;            // 48
    const slp_builtin_t *builtins;  // the built-in amps in AmpId order (id + name)
    uint8_t builtin_count;
    uint8_t role;                   // SLP_ROLE_SEED (0 means SLP_ROLE_SEED)

    // Uploaded models are received into this buffer (at least 7484 bytes for A2-Lite). It is a
    // staging buffer: the next upload overwrites it, so on_select_model must copy the weights
    // into the buffer the engine runs from (while audio is stopped). NULL = refuse uploads.
    uint8_t *model_buf;
    uint32_t model_buf_size;

    // Hooks, called from slp_rx_feed() as frames arrive. Any may be NULL.
    void (*on_param)(void *ctx, uint16_t id, int16_t value);      // UI units, knobs 0..100
    void (*on_fx)(void *ctx, uint8_t fx, bool on, uint8_t model);  // SLP_FX_*
    void (*on_chain)(void *ctx, const uint8_t *fx, uint8_t len);   // CAB is implied last
    void (*on_master)(void *ctx, uint8_t volume, bool muted);      // mute must apply at once
    // weights: the received model when m->kind is SLP_MODEL_UPLOADED and it was stored with
    // that CRC32; NULL if the Seed doesn't hold it (report SLP_STATUS_MODEL_FAILED; the ESP32
    // then uploads it again). Load models with audio stopped.
    void (*on_select_model)(void *ctx, const slp_select_model_t *m, const uint8_t *weights);
    void (*on_link)(void *ctx, bool up);                          // the ESP32 appeared / went silent
} slp_rx_config_t;

typedef struct {
    slp_rx_config_t cfg;
    slp_decoder_t dec;
    uint8_t seq;
    bool peer_heard, peer_known;
    uint32_t peer_boot_id;
    uint32_t last_rx_ms, next_hello_ms, next_heartbeat_ms;
    bool in_snapshot;
    uint16_t snap_id, applied_snapshot;
    uint8_t snap_frames;
    bool up_active;
    uint8_t up_transfer;
    uint32_t up_size, up_crc;
    bool stored_valid;
    uint32_t stored_hash;
} slp_receiver_t;

void slp_rx_init(slp_receiver_t *r, const slp_rx_config_t *cfg);
// Received UART bytes (main loop). Decodes frames, answers, and calls the hooks.
void slp_rx_feed(slp_receiver_t *r, const uint8_t *data, size_t len);
// Call every main-loop pass: HELLO until the ESP32 is heard, HEARTBEAT at 1 Hz, link timeout.
void slp_rx_poll(slp_receiver_t *r);
// Telemetry for the ESP32: STATUS at about 2 Hz, METERS at about 30 Hz. STATUS's
// applied_snapshot_id is filled in from the receiver's own record.
void slp_rx_send_status(slp_receiver_t *r, const slp_status_t *st);
void slp_rx_send_meters(slp_receiver_t *r, const slp_meters_t *m);
void slp_rx_send_log(slp_receiver_t *r, const char *text);
bool slp_rx_link_up(const slp_receiver_t *r);

#ifdef __cplusplus
}
#endif
#endif
