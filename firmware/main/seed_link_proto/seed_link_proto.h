/*
 * seed_link_proto: the ESP32 touchscreen <-> Daisy Seed3 link protocol, version 1.
 *
 * Pure C99. No malloc, no OS, no ESP-IDF or libDaisy dependency, so the same two files
 * (seed_link_proto.h/.c) build in the ESP32 firmware, in the host unit tests and in the Seed's
 * C++20 firmware unchanged. The full specification, with byte layouts and hex examples, is
 * docs/SEED_LINK_PROTOCOL.md in the esp32-touchscreen-ui repo.
 *
 * Wire format: every frame is COBS-encoded and sent between two 0x00 bytes (0x00, COBS data,
 * 0x00). Empty frames between delimiters are ignored. Decoded:
 *
 *   ver u8 | type u8 | seq u8 | flags u8 | len u16 | payload[len] | crc16 u16
 *
 * Little-endian. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over header and payload.
 * Anything that fails to decode is counted and dropped; the decoder resynchronises on the next
 * 0x00, so line noise and the ESP32's boot banner are harmless.
 */
#ifndef SEED_LINK_PROTO_H
#define SEED_LINK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_VERSION        1
#define SLP_HEADER_LEN     6
#define SLP_CRC_LEN        2
#define SLP_MAX_PAYLOAD    256
#define SLP_MAX_FRAME      (SLP_HEADER_LEN + SLP_MAX_PAYLOAD + SLP_CRC_LEN)
// COBS adds at most one byte per 254 plus one; a 0x00 delimiter goes before and after.
#define SLP_MAX_ENCODED    (SLP_MAX_FRAME + SLP_MAX_FRAME / 254 + 3)

// Header flags
#define SLP_FLAG_ACK_REQ   0x01  // the receiver must answer with ACK (only MODEL_* frames set it)

// Message types
enum {
    // Link management, both directions
    SLP_HELLO          = 0x01,
    SLP_HEARTBEAT      = 0x02,
    SLP_PING           = 0x03,
    SLP_PONG           = 0x04,
    SLP_ACK            = 0x05,
    SLP_LOG            = 0x06,
    // ESP32 -> Seed: state (idempotent, latest value wins, not acknowledged)
    SLP_SET_PARAMS     = 0x10,
    SLP_SET_FX_STATE   = 0x11,
    SLP_SET_CHAIN      = 0x12,
    SLP_SET_MASTER     = 0x13,
    SLP_SELECT_MODEL   = 0x14,
    SLP_SNAPSHOT_BEGIN = 0x15,
    SLP_SNAPSHOT_END   = 0x16,
    // Seed -> ESP32: telemetry
    SLP_STATUS         = 0x20,
    SLP_METERS         = 0x21,
    // ESP32 -> Seed model upload, and the Seed's answer
    SLP_MODEL_BEGIN    = 0x30,
    SLP_MODEL_CHUNK    = 0x31,
    SLP_MODEL_COMMIT   = 0x32,
    SLP_MODEL_RESULT   = 0x33,
    SLP_MODEL_ABORT    = 0x34,
};

// HELLO roles
enum {
    SLP_ROLE_UI        = 1,  // the ESP32 touchscreen
    SLP_ROLE_SEED      = 2,  // the real Daisy Seed3
    SLP_ROLE_MOCK_SEED = 3,  // tools/mock_seed.py: everything it reports is simulated
};

// ACK / MODEL_RESULT status codes
enum {
    SLP_OK             = 0,
    SLP_ERR_STATE      = 1,  // unexpected in the current state (e.g. CHUNK without BEGIN)
    SLP_ERR_CRC        = 2,  // model CRC32 mismatch
    SLP_ERR_RANGE      = 3,  // offset/length out of range
    SLP_ERR_NO_MEMORY  = 4,
    SLP_ERR_BUSY       = 5,
    SLP_ERR_REJECTED   = 6,  // model failed to load (shape, sample rate, Reset threw)
    SLP_ERR_TIMEOUT    = 7,
};

// SELECT_MODEL kinds
enum {
    SLP_MODEL_BUILTIN  = 1,  // builtin_id = the Seed's AmpId (1..)
    SLP_MODEL_UPLOADED = 2,  // hash = CRC32 of the uploaded packed weights
    SLP_MODEL_BYPASS   = 3,  // amp bypassed (no model in the path)
};

// STATUS flags
#define SLP_STATUS_BYPASS        0x01
#define SLP_STATUS_MODEL_FAILED  0x02
#define SLP_STATUS_AUDIO_RUNNING 0x04

// SNAPSHOT reasons
enum { SLP_SNAP_HELLO = 1, SLP_SNAP_PRESET = 2, SLP_SNAP_BOOT = 3, SLP_SNAP_PERIODIC = 4, SLP_SNAP_RESYNC = 5 };

// Effect wire ids (stable, never renumbered). CAB is always last in the chain and never sent
// in SET_CHAIN.
enum {
    SLP_FX_GATE = 1, SLP_FX_COMP, SLP_FX_EQ, SLP_FX_DRIVE, SLP_FX_AMP, SLP_FX_OCTAVE,
    SLP_FX_CHORUS, SLP_FX_PHASER, SLP_FX_TREMOLO, SLP_FX_DELAY, SLP_FX_REVERB, SLP_FX_CAB,
    SLP_FX_COUNT = 12,
};

#define SLP_NAME_LEN        24   // built-in model name in HELLO, NUL-padded, not terminated if full
#define SLP_FW_LEN          16
#define SLP_MODEL_NAME_LEN  32
#define SLP_MAX_BUILTINS    8
#define SLP_MAX_PARAMS      63   // per SET_PARAMS frame: 1 + 63 * 4 = 253 bytes
#define SLP_CHUNK_MAX       192  // MODEL_CHUNK data bytes
#define SLP_METER_SILENCE   (-12000)  // centi-dBFS reported for digital silence

// ---------------------------------------------------------------------------------------------
// Checksums and COBS

uint16_t slp_crc16(const uint8_t *data, size_t len);
// CRC-32 (IEEE 802.3, reflected, as zlib/binascii.crc32). Start with crc = 0; chainable.
uint32_t slp_crc32(uint32_t crc, const uint8_t *data, size_t len);

// COBS without the trailing 0x00. Encoded length <= len + len / 254 + 1. Returns bytes written,
// or 0 if out_cap is too small.
size_t slp_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);
// Returns decoded length, or 0 on malformed input or overflow. in must not contain 0x00.
size_t slp_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

// ---------------------------------------------------------------------------------------------
// Frames

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t flags;
    uint16_t len;
    const uint8_t *payload;  // points into the decoder's buffer; valid until the next push
} slp_frame_t;

// Builds a complete wire frame (0x00, COBS, 0x00) into out. Returns bytes written, 0 on error
// (payload too long or out too small; out_cap >= SLP_MAX_ENCODED always suffices).
size_t slp_frame_encode(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t *payload,
                        uint16_t len, uint8_t *out, size_t out_cap);

typedef struct {
    uint8_t raw[SLP_MAX_ENCODED];
    size_t n;
    bool overflow;           // current frame exceeded the buffer: drop it at the next 0x00
    uint8_t frame[SLP_MAX_FRAME];
    uint32_t frames_ok;
    uint32_t err_cobs;       // malformed COBS
    uint32_t err_short;      // shorter than header + CRC, or len field disagrees
    uint32_t err_version;
    uint32_t err_crc;
    uint32_t err_overflow;   // longer than any legal frame
} slp_decoder_t;

void slp_decoder_init(slp_decoder_t *d);
// Feed one received byte. Returns true and fills *out when a valid frame completes.
bool slp_decoder_push(slp_decoder_t *d, uint8_t byte, slp_frame_t *out);
uint32_t slp_decoder_errors(const slp_decoder_t *d);

// ---------------------------------------------------------------------------------------------
// Messages. Each has pack (returns payload length, 0 if cap is too small or the struct is
// invalid) and unpack (returns false unless the payload has exactly the right length and
// valid fields). Unpack never reads past len.

typedef struct {
    uint8_t id;                    // AmpId on the Seed
    char name[SLP_NAME_LEN + 1];   // NUL-terminated after unpack
} slp_builtin_t;

typedef struct {
    uint8_t proto_version;
    uint8_t role;
    uint32_t boot_id;              // random per boot; a new value means the peer rebooted
    char fw_version[SLP_FW_LEN + 1];
    uint32_t sample_rate_hz;       // 0 from the UI side
    uint16_t block_size;           // 0 from the UI side
    uint8_t builtin_count;
    slp_builtin_t builtins[SLP_MAX_BUILTINS];
} slp_hello_t;
size_t slp_hello_pack(const slp_hello_t *m, uint8_t *buf, size_t cap);
bool slp_hello_unpack(const uint8_t *p, size_t len, slp_hello_t *m);

typedef struct {
    uint32_t uptime_ms;
    uint32_t rx_frames;            // valid frames this side has received since boot
    uint32_t rx_errors;            // frames this side dropped since boot
} slp_heartbeat_t;
size_t slp_heartbeat_pack(const slp_heartbeat_t *m, uint8_t *buf, size_t cap);
bool slp_heartbeat_unpack(const uint8_t *p, size_t len, slp_heartbeat_t *m);

typedef struct {
    uint32_t token;
    uint32_t t_send_us;            // sender's clock, echoed back unchanged in PONG
} slp_ping_t;                      // also the PONG payload
size_t slp_ping_pack(const slp_ping_t *m, uint8_t *buf, size_t cap);
bool slp_ping_unpack(const uint8_t *p, size_t len, slp_ping_t *m);

typedef struct {
    uint8_t acked_type;
    uint8_t acked_seq;
    uint8_t status;
} slp_ack_t;
size_t slp_ack_pack(const slp_ack_t *m, uint8_t *buf, size_t cap);
bool slp_ack_unpack(const uint8_t *p, size_t len, slp_ack_t *m);

typedef struct {
    uint16_t id;
    int16_t value;                 // UI units: knobs 0..100
} slp_param_t;
typedef struct {
    uint8_t count;
    slp_param_t params[SLP_MAX_PARAMS];
} slp_set_params_t;
size_t slp_set_params_pack(const slp_set_params_t *m, uint8_t *buf, size_t cap);
bool slp_set_params_unpack(const uint8_t *p, size_t len, slp_set_params_t *m);

typedef struct {
    uint8_t fx;                    // SLP_FX_*
    uint8_t on;                    // 0 = bypassed
    uint8_t model;                 // the UI's model index for that effect (0 when it has none)
} slp_fx_entry_t;
typedef struct {
    uint8_t count;
    slp_fx_entry_t fx[SLP_FX_COUNT];
} slp_set_fx_state_t;
size_t slp_set_fx_state_pack(const slp_set_fx_state_t *m, uint8_t *buf, size_t cap);
bool slp_set_fx_state_unpack(const uint8_t *p, size_t len, slp_set_fx_state_t *m);

typedef struct {
    uint8_t len;
    uint8_t fx[SLP_FX_COUNT - 1];  // processing order, CAB excluded (always last)
} slp_set_chain_t;
size_t slp_set_chain_pack(const slp_set_chain_t *m, uint8_t *buf, size_t cap);
bool slp_set_chain_unpack(const uint8_t *p, size_t len, slp_set_chain_t *m);

typedef struct {
    uint8_t volume;                // 0..100
    uint8_t muted;                 // 1 = hard mute; volume is kept so un-mute restores it
} slp_set_master_t;
size_t slp_set_master_pack(const slp_set_master_t *m, uint8_t *buf, size_t cap);
bool slp_set_master_unpack(const uint8_t *p, size_t len, slp_set_master_t *m);

typedef struct {
    uint8_t kind;                  // SLP_MODEL_*
    uint8_t builtin_id;            // for SLP_MODEL_BUILTIN, else 0
    uint32_t hash;                 // for SLP_MODEL_UPLOADED, else 0
} slp_select_model_t;
size_t slp_select_model_pack(const slp_select_model_t *m, uint8_t *buf, size_t cap);
bool slp_select_model_unpack(const uint8_t *p, size_t len, slp_select_model_t *m);

typedef struct {
    uint16_t snapshot_id;
    uint8_t reason;                // SLP_SNAP_* on BEGIN; on END, the number of state frames sent between
} slp_snapshot_t;
size_t slp_snapshot_pack(const slp_snapshot_t *m, uint8_t *buf, size_t cap);
bool slp_snapshot_unpack(const uint8_t *p, size_t len, slp_snapshot_t *m);

typedef struct {
    uint16_t cpu_avg_x10;          // DSP load in 0.1 % of the audio block budget
    uint16_t cpu_peak_x10;
    uint32_t overruns;             // since boot
    uint8_t model_kind;            // SLP_MODEL_* active now
    uint8_t builtin_id;
    uint32_t model_hash;
    uint8_t flags;                 // SLP_STATUS_*
    uint16_t applied_snapshot_id;  // last snapshot fully applied
} slp_status_t;
size_t slp_status_pack(const slp_status_t *m, uint8_t *buf, size_t cap);
bool slp_status_unpack(const uint8_t *p, size_t len, slp_status_t *m);

typedef struct {
    int16_t in_peak_cdb;           // input peak since the last METERS, centi-dBFS (-600 = -6 dBFS)
    int16_t out_peak_cdb;
    uint32_t clip_count;           // since boot
} slp_meters_t;
size_t slp_meters_pack(const slp_meters_t *m, uint8_t *buf, size_t cap);
bool slp_meters_unpack(const uint8_t *p, size_t len, slp_meters_t *m);

typedef struct {
    uint8_t transfer_id;
    uint16_t weight_count;         // 1871 for A2-Lite
    uint32_t byte_count;           // weight_count * 4
    uint32_t crc32;                // slp_crc32 over the packed little-endian float32 weights
    char name[SLP_MODEL_NAME_LEN + 1];
} slp_model_begin_t;
size_t slp_model_begin_pack(const slp_model_begin_t *m, uint8_t *buf, size_t cap);
bool slp_model_begin_unpack(const uint8_t *p, size_t len, slp_model_begin_t *m);

typedef struct {
    uint8_t transfer_id;
    uint32_t offset;
    uint8_t size;                  // 1..SLP_CHUNK_MAX
    const uint8_t *data;           // pack reads, unpack points into the payload
} slp_model_chunk_t;
size_t slp_model_chunk_pack(const slp_model_chunk_t *m, uint8_t *buf, size_t cap);
bool slp_model_chunk_unpack(const uint8_t *p, size_t len, slp_model_chunk_t *m);

typedef struct {
    uint8_t transfer_id;
    uint8_t status;                // MODEL_RESULT and MODEL_ABORT; unused (0) in MODEL_COMMIT
    uint32_t crc32;                // MODEL_RESULT: what the Seed computed; else 0
} slp_model_end_t;                 // payload of MODEL_COMMIT, MODEL_RESULT and MODEL_ABORT
size_t slp_model_end_pack(const slp_model_end_t *m, uint8_t *buf, size_t cap);
bool slp_model_end_unpack(const uint8_t *p, size_t len, slp_model_end_t *m);

#ifdef __cplusplus
}
#endif
#endif
