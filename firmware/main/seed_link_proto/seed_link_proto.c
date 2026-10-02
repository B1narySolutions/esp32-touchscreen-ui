/*
 * seed_link_proto: see seed_link_proto.h. Pure C99, no allocation.
 */
#include "seed_link_proto.h"

#include <string.h>

// ---------------------------------------------------------------------------------------------
// Little-endian helpers. Byte-wise so they work on any alignment and either endianness.

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Fixed-width text field: copied NUL-padded; on unpack always NUL-terminated.
static void put_text(uint8_t *p, const char *s, size_t width) {
    size_t n = 0;
    while (n < width && s[n]) n++;
    memcpy(p, s, n);
    memset(p + n, 0, width - n);
}
static void get_text(const uint8_t *p, size_t width, char *out) {
    size_t n = 0;
    while (n < width && p[n]) n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

// ---------------------------------------------------------------------------------------------
// Checksums

uint16_t slp_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint32_t slp_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    // Bitwise: the Seed only runs this once per uploaded model, and a table costs 1 KB.
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

// ---------------------------------------------------------------------------------------------
// COBS

size_t slp_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap) {
    if (out_cap < len + len / 254 + 1) return 0;
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFF) {
                // Full 254-byte block. Open the next one only if input remains (canonical COBS).
                out[code_at] = code;
                if (i + 1 == len) return o;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return o;
}

size_t slp_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap) {
    size_t i = 0, o = 0;
    while (i < len) {
        const uint8_t code = in[i++];
        if (code == 0) return 0;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len || in[i] == 0 || o >= out_cap) return 0;
            out[o++] = in[i++];
        }
        // A code below 0xFF stands for a zero, except after the last block.
        if (code != 0xFF && i < len) {
            if (o >= out_cap) return 0;
            out[o++] = 0;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------------------------
// Frames

size_t slp_frame_encode(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t *payload,
                        uint16_t len, uint8_t *out, size_t out_cap) {
    if (len > SLP_MAX_PAYLOAD || (len && !payload)) return 0;
    uint8_t frame[SLP_MAX_FRAME];
    frame[0] = SLP_VERSION;
    frame[1] = type;
    frame[2] = seq;
    frame[3] = flags;
    put_u16(frame + 4, len);
    if (len) memcpy(frame + SLP_HEADER_LEN, payload, len);
    const size_t body = SLP_HEADER_LEN + (size_t)len;
    put_u16(frame + body, slp_crc16(frame, body));

    // Leading 0x00: whatever junk preceded this frame on the line (a peer's boot output, a
    // frame cut short by a reset) is terminated there and can't swallow this frame.
    if (out_cap < 2) return 0;
    out[0] = 0x00;
    size_t n = slp_cobs_encode(frame, body + SLP_CRC_LEN, out + 1, out_cap - 2);
    if (n == 0) return 0;
    n++;
    out[n++] = 0x00;
    return n;
}

void slp_decoder_init(slp_decoder_t *d) { memset(d, 0, sizeof(*d)); }

uint32_t slp_decoder_errors(const slp_decoder_t *d) {
    return d->err_cobs + d->err_short + d->err_version + d->err_crc + d->err_overflow;
}

bool slp_decoder_push(slp_decoder_t *d, uint8_t byte, slp_frame_t *out) {
    if (byte != 0x00) {
        if (d->n < sizeof(d->raw)) d->raw[d->n++] = byte;
        else d->overflow = true;
        return false;
    }

    // Delimiter: try to decode what was collected, then start over.
    const size_t n = d->n;
    const bool overflow = d->overflow;
    d->n = 0;
    d->overflow = false;
    if (n == 0) return false; // back-to-back delimiters are allowed as idle filler
    if (overflow) { d->err_overflow++; return false; }

    const size_t len = slp_cobs_decode(d->raw, n, d->frame, sizeof(d->frame));
    if (len == 0) { d->err_cobs++; return false; }
    if (len < SLP_HEADER_LEN + SLP_CRC_LEN) { d->err_short++; return false; }
    const uint16_t plen = get_u16(d->frame + 4);
    if ((size_t)plen + SLP_HEADER_LEN + SLP_CRC_LEN != len) { d->err_short++; return false; }
    if (slp_crc16(d->frame, len - SLP_CRC_LEN) != get_u16(d->frame + len - SLP_CRC_LEN)) { d->err_crc++; return false; }
    if (d->frame[0] != SLP_VERSION) { d->err_version++; return false; }

    out->type = d->frame[1];
    out->seq = d->frame[2];
    out->flags = d->frame[3];
    out->len = plen;
    out->payload = d->frame + SLP_HEADER_LEN;
    d->frames_ok++;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Messages

#define HELLO_FIXED 29
#define BUILTIN_LEN (1 + SLP_NAME_LEN)

size_t slp_hello_pack(const slp_hello_t *m, uint8_t *buf, size_t cap) {
    if (m->builtin_count > SLP_MAX_BUILTINS) return 0;
    const size_t len = HELLO_FIXED + (size_t)m->builtin_count * BUILTIN_LEN;
    if (cap < len) return 0;
    buf[0] = m->proto_version;
    buf[1] = m->role;
    put_u32(buf + 2, m->boot_id);
    put_text(buf + 6, m->fw_version, SLP_FW_LEN);
    put_u32(buf + 22, m->sample_rate_hz);
    put_u16(buf + 26, m->block_size);
    buf[28] = m->builtin_count;
    for (int i = 0; i < m->builtin_count; i++) {
        uint8_t *b = buf + HELLO_FIXED + i * BUILTIN_LEN;
        b[0] = m->builtins[i].id;
        put_text(b + 1, m->builtins[i].name, SLP_NAME_LEN);
    }
    return len;
}

bool slp_hello_unpack(const uint8_t *p, size_t len, slp_hello_t *m) {
    if (len < HELLO_FIXED) return false;
    const uint8_t count = p[28];
    if (count > SLP_MAX_BUILTINS || len != HELLO_FIXED + (size_t)count * BUILTIN_LEN) return false;
    if (p[1] < SLP_ROLE_UI || p[1] > SLP_ROLE_MOCK_SEED) return false;
    m->proto_version = p[0];
    m->role = p[1];
    m->boot_id = get_u32(p + 2);
    get_text(p + 6, SLP_FW_LEN, m->fw_version);
    m->sample_rate_hz = get_u32(p + 22);
    m->block_size = get_u16(p + 26);
    m->builtin_count = count;
    for (int i = 0; i < count; i++) {
        const uint8_t *b = p + HELLO_FIXED + i * BUILTIN_LEN;
        m->builtins[i].id = b[0];
        get_text(b + 1, SLP_NAME_LEN, m->builtins[i].name);
    }
    return true;
}

size_t slp_heartbeat_pack(const slp_heartbeat_t *m, uint8_t *buf, size_t cap) {
    if (cap < 12) return 0;
    put_u32(buf, m->uptime_ms);
    put_u32(buf + 4, m->rx_frames);
    put_u32(buf + 8, m->rx_errors);
    return 12;
}

bool slp_heartbeat_unpack(const uint8_t *p, size_t len, slp_heartbeat_t *m) {
    if (len != 12) return false;
    m->uptime_ms = get_u32(p);
    m->rx_frames = get_u32(p + 4);
    m->rx_errors = get_u32(p + 8);
    return true;
}

size_t slp_ping_pack(const slp_ping_t *m, uint8_t *buf, size_t cap) {
    if (cap < 8) return 0;
    put_u32(buf, m->token);
    put_u32(buf + 4, m->t_send_us);
    return 8;
}

bool slp_ping_unpack(const uint8_t *p, size_t len, slp_ping_t *m) {
    if (len != 8) return false;
    m->token = get_u32(p);
    m->t_send_us = get_u32(p + 4);
    return true;
}

size_t slp_ack_pack(const slp_ack_t *m, uint8_t *buf, size_t cap) {
    if (cap < 3) return 0;
    buf[0] = m->acked_type;
    buf[1] = m->acked_seq;
    buf[2] = m->status;
    return 3;
}

bool slp_ack_unpack(const uint8_t *p, size_t len, slp_ack_t *m) {
    if (len != 3) return false;
    m->acked_type = p[0];
    m->acked_seq = p[1];
    m->status = p[2];
    return true;
}

size_t slp_set_params_pack(const slp_set_params_t *m, uint8_t *buf, size_t cap) {
    if (m->count == 0 || m->count > SLP_MAX_PARAMS) return 0;
    const size_t len = 1 + (size_t)m->count * 4;
    if (cap < len) return 0;
    buf[0] = m->count;
    for (int i = 0; i < m->count; i++) {
        put_u16(buf + 1 + i * 4, m->params[i].id);
        put_u16(buf + 3 + i * 4, (uint16_t)m->params[i].value);
    }
    return len;
}

bool slp_set_params_unpack(const uint8_t *p, size_t len, slp_set_params_t *m) {
    if (len < 1 || p[0] == 0 || p[0] > SLP_MAX_PARAMS || len != 1 + (size_t)p[0] * 4) return false;
    m->count = p[0];
    for (int i = 0; i < m->count; i++) {
        m->params[i].id = get_u16(p + 1 + i * 4);
        m->params[i].value = (int16_t)get_u16(p + 3 + i * 4);
    }
    return true;
}

static bool valid_fx(uint8_t fx) { return fx >= SLP_FX_GATE && fx <= SLP_FX_CAB; }

size_t slp_set_fx_state_pack(const slp_set_fx_state_t *m, uint8_t *buf, size_t cap) {
    if (m->count == 0 || m->count > SLP_FX_COUNT) return 0;
    const size_t len = 1 + (size_t)m->count * 3;
    if (cap < len) return 0;
    buf[0] = m->count;
    for (int i = 0; i < m->count; i++) {
        if (!valid_fx(m->fx[i].fx)) return 0;
        buf[1 + i * 3] = m->fx[i].fx;
        buf[2 + i * 3] = m->fx[i].on ? 1 : 0;
        buf[3 + i * 3] = m->fx[i].model;
    }
    return len;
}

bool slp_set_fx_state_unpack(const uint8_t *p, size_t len, slp_set_fx_state_t *m) {
    if (len < 1 || p[0] == 0 || p[0] > SLP_FX_COUNT || len != 1 + (size_t)p[0] * 3) return false;
    m->count = p[0];
    for (int i = 0; i < m->count; i++) {
        const uint8_t *e = p + 1 + i * 3;
        if (!valid_fx(e[0]) || e[1] > 1) return false;
        m->fx[i].fx = e[0];
        m->fx[i].on = e[1];
        m->fx[i].model = e[2];
    }
    return true;
}

// Chain entries: movable effects only (CAB excluded), each at most once.
static bool valid_chain(const uint8_t *fx, uint8_t len) {
    if (len == 0 || len > SLP_FX_COUNT - 1) return false;
    uint32_t seen = 0;
    for (int i = 0; i < len; i++) {
        if (fx[i] < SLP_FX_GATE || fx[i] >= SLP_FX_CAB || (seen & (1u << fx[i]))) return false;
        seen |= 1u << fx[i];
    }
    return true;
}

size_t slp_set_chain_pack(const slp_set_chain_t *m, uint8_t *buf, size_t cap) {
    if (!valid_chain(m->fx, m->len) || cap < 1u + m->len) return 0;
    buf[0] = m->len;
    memcpy(buf + 1, m->fx, m->len);
    return 1u + m->len;
}

bool slp_set_chain_unpack(const uint8_t *p, size_t len, slp_set_chain_t *m) {
    if (len < 1 || len != 1u + p[0] || !valid_chain(p + 1, p[0])) return false;
    m->len = p[0];
    memcpy(m->fx, p + 1, m->len);
    return true;
}

size_t slp_set_master_pack(const slp_set_master_t *m, uint8_t *buf, size_t cap) {
    if (cap < 2 || m->volume > 100 || m->muted > 1) return 0;
    buf[0] = m->volume;
    buf[1] = m->muted;
    return 2;
}

bool slp_set_master_unpack(const uint8_t *p, size_t len, slp_set_master_t *m) {
    if (len != 2 || p[0] > 100 || p[1] > 1) return false;
    m->volume = p[0];
    m->muted = p[1];
    return true;
}

size_t slp_select_model_pack(const slp_select_model_t *m, uint8_t *buf, size_t cap) {
    if (cap < 6 || m->kind < SLP_MODEL_BUILTIN || m->kind > SLP_MODEL_BYPASS) return 0;
    buf[0] = m->kind;
    buf[1] = m->builtin_id;
    put_u32(buf + 2, m->hash);
    return 6;
}

bool slp_select_model_unpack(const uint8_t *p, size_t len, slp_select_model_t *m) {
    if (len != 6 || p[0] < SLP_MODEL_BUILTIN || p[0] > SLP_MODEL_BYPASS) return false;
    m->kind = p[0];
    m->builtin_id = p[1];
    m->hash = get_u32(p + 2);
    return true;
}

size_t slp_snapshot_pack(const slp_snapshot_t *m, uint8_t *buf, size_t cap) {
    if (cap < 3) return 0;
    put_u16(buf, m->snapshot_id);
    buf[2] = m->reason;
    return 3;
}

bool slp_snapshot_unpack(const uint8_t *p, size_t len, slp_snapshot_t *m) {
    if (len != 3) return false;
    m->snapshot_id = get_u16(p);
    m->reason = p[2];
    return true;
}

size_t slp_status_pack(const slp_status_t *m, uint8_t *buf, size_t cap) {
    if (cap < 17) return 0;
    put_u16(buf, m->cpu_avg_x10);
    put_u16(buf + 2, m->cpu_peak_x10);
    put_u32(buf + 4, m->overruns);
    buf[8] = m->model_kind;
    buf[9] = m->builtin_id;
    put_u32(buf + 10, m->model_hash);
    buf[14] = m->flags;
    put_u16(buf + 15, m->applied_snapshot_id);
    return 17;
}

bool slp_status_unpack(const uint8_t *p, size_t len, slp_status_t *m) {
    if (len != 17) return false;
    m->cpu_avg_x10 = get_u16(p);
    m->cpu_peak_x10 = get_u16(p + 2);
    m->overruns = get_u32(p + 4);
    m->model_kind = p[8];
    m->builtin_id = p[9];
    m->model_hash = get_u32(p + 10);
    m->flags = p[14];
    m->applied_snapshot_id = get_u16(p + 15);
    return true;
}

size_t slp_meters_pack(const slp_meters_t *m, uint8_t *buf, size_t cap) {
    if (cap < 8) return 0;
    put_u16(buf, (uint16_t)m->in_peak_cdb);
    put_u16(buf + 2, (uint16_t)m->out_peak_cdb);
    put_u32(buf + 4, m->clip_count);
    return 8;
}

bool slp_meters_unpack(const uint8_t *p, size_t len, slp_meters_t *m) {
    if (len != 8) return false;
    m->in_peak_cdb = (int16_t)get_u16(p);
    m->out_peak_cdb = (int16_t)get_u16(p + 2);
    m->clip_count = get_u32(p + 4);
    return true;
}

#define MODEL_BEGIN_LEN (11 + SLP_MODEL_NAME_LEN)

size_t slp_model_begin_pack(const slp_model_begin_t *m, uint8_t *buf, size_t cap) {
    if (cap < MODEL_BEGIN_LEN || m->byte_count != (uint32_t)m->weight_count * 4u) return 0;
    buf[0] = m->transfer_id;
    put_u16(buf + 1, m->weight_count);
    put_u32(buf + 3, m->byte_count);
    put_u32(buf + 7, m->crc32);
    put_text(buf + 11, m->name, SLP_MODEL_NAME_LEN);
    return MODEL_BEGIN_LEN;
}

bool slp_model_begin_unpack(const uint8_t *p, size_t len, slp_model_begin_t *m) {
    if (len != MODEL_BEGIN_LEN) return false;
    m->transfer_id = p[0];
    m->weight_count = get_u16(p + 1);
    m->byte_count = get_u32(p + 3);
    m->crc32 = get_u32(p + 7);
    get_text(p + 11, SLP_MODEL_NAME_LEN, m->name);
    return m->byte_count == (uint32_t)m->weight_count * 4u;
}

size_t slp_model_chunk_pack(const slp_model_chunk_t *m, uint8_t *buf, size_t cap) {
    if (m->size == 0 || m->size > SLP_CHUNK_MAX || !m->data || cap < 5u + m->size) return 0;
    buf[0] = m->transfer_id;
    put_u32(buf + 1, m->offset);
    memcpy(buf + 5, m->data, m->size);
    return 5u + m->size;
}

bool slp_model_chunk_unpack(const uint8_t *p, size_t len, slp_model_chunk_t *m) {
    if (len < 6 || len > 5u + SLP_CHUNK_MAX) return false;
    m->transfer_id = p[0];
    m->offset = get_u32(p + 1);
    m->size = (uint8_t)(len - 5);
    m->data = p + 5;
    return true;
}

size_t slp_model_end_pack(const slp_model_end_t *m, uint8_t *buf, size_t cap) {
    if (cap < 6) return 0;
    buf[0] = m->transfer_id;
    buf[1] = m->status;
    put_u32(buf + 2, m->crc32);
    return 6;
}

bool slp_model_end_unpack(const uint8_t *p, size_t len, slp_model_end_t *m) {
    if (len != 6) return false;
    m->transfer_id = p[0];
    m->status = p[1];
    m->crc32 = get_u32(p + 2);
    return true;
}
