/*
 * slp_receiver: see slp_receiver.h. Behaviour follows docs/SEED_LINK_PROTOCOL.md section 3.
 */
#include "slp_receiver.h"

#include <string.h>

#define HELLO_PERIOD_MS     500
#define HEARTBEAT_PERIOD_MS 1000
#define LINK_TIMEOUT_MS     3000

static void send(slp_receiver_t *r, uint8_t type, const uint8_t *p, size_t len) {
    uint8_t wire[SLP_MAX_ENCODED];
    const size_t n = slp_frame_encode(type, r->seq++, 0, p, (uint16_t)len, wire, sizeof(wire));
    if (n) r->cfg.send(r->cfg.ctx, wire, n);
}

static void send_hello(slp_receiver_t *r) {
    slp_hello_t h;
    memset(&h, 0, sizeof(h));
    h.proto_version = SLP_VERSION;
    h.role = r->cfg.role ? r->cfg.role : SLP_ROLE_SEED;
    h.boot_id = r->cfg.boot_id;
    if (r->cfg.fw_version) strncpy(h.fw_version, r->cfg.fw_version, SLP_FW_LEN);
    h.sample_rate_hz = r->cfg.sample_rate_hz;
    h.block_size = r->cfg.block_size;
    h.builtin_count = r->cfg.builtin_count > SLP_MAX_BUILTINS ? SLP_MAX_BUILTINS : r->cfg.builtin_count;
    for (int i = 0; i < h.builtin_count; i++) h.builtins[i] = r->cfg.builtins[i];
    uint8_t p[SLP_MAX_PAYLOAD];
    send(r, SLP_HELLO, p, slp_hello_pack(&h, p, sizeof(p)));
}

static void ack(slp_receiver_t *r, const slp_frame_t *f, uint8_t status) {
    const slp_ack_t a = { f->type, f->seq, status };
    uint8_t p[3];
    send(r, SLP_ACK, p, slp_ack_pack(&a, p, sizeof(p)));
}

void slp_rx_init(slp_receiver_t *r, const slp_rx_config_t *cfg) {
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    slp_decoder_init(&r->dec);
    r->next_hello_ms = r->next_heartbeat_ms = cfg->now_ms(cfg->ctx);
}

bool slp_rx_link_up(const slp_receiver_t *r) { return r->peer_heard; }

static void set_link(slp_receiver_t *r, bool up) {
    if (r->peer_heard == up) return;
    r->peer_heard = up;
    if (r->cfg.on_link) r->cfg.on_link(r->cfg.ctx, up);
}

// State frames inside a snapshot are counted, so SNAPSHOT_END can confirm completeness.
static void state_frame(slp_receiver_t *r) {
    if (r->in_snapshot && r->snap_frames < 255) r->snap_frames++;
}

static void handle(slp_receiver_t *r, const slp_frame_t *f) {
    void *ctx = r->cfg.ctx;
    switch (f->type) {
    case SLP_HELLO: {
        slp_hello_t h;
        if (!slp_hello_unpack(f->payload, f->len, &h) || h.role != SLP_ROLE_UI) return;
        // Answer each new ESP32 boot once (it may have missed our HELLOs while it booted).
        if (!r->peer_known || h.boot_id != r->peer_boot_id) send_hello(r);
        r->peer_known = true;
        r->peer_boot_id = h.boot_id;
        set_link(r, true);
        return;
    }
    case SLP_PING:
        send(r, SLP_PONG, f->payload, f->len);
        return;
    case SLP_SET_PARAMS: {
        slp_set_params_t m;
        if (!slp_set_params_unpack(f->payload, f->len, &m)) return;
        state_frame(r);
        for (int i = 0; r->cfg.on_param && i < m.count; i++) r->cfg.on_param(ctx, m.params[i].id, m.params[i].value);
        return;
    }
    case SLP_SET_FX_STATE: {
        slp_set_fx_state_t m;
        if (!slp_set_fx_state_unpack(f->payload, f->len, &m)) return;
        state_frame(r);
        for (int i = 0; r->cfg.on_fx && i < m.count; i++) r->cfg.on_fx(ctx, m.fx[i].fx, m.fx[i].on != 0, m.fx[i].model);
        return;
    }
    case SLP_SET_CHAIN: {
        slp_set_chain_t m;
        if (!slp_set_chain_unpack(f->payload, f->len, &m)) return;
        state_frame(r);
        if (r->cfg.on_chain) r->cfg.on_chain(ctx, m.fx, m.len);
        return;
    }
    case SLP_SET_MASTER: {
        slp_set_master_t m;
        if (!slp_set_master_unpack(f->payload, f->len, &m)) return;
        state_frame(r);
        if (r->cfg.on_master) r->cfg.on_master(ctx, m.volume, m.muted != 0);
        return;
    }
    case SLP_SELECT_MODEL: {
        slp_select_model_t m;
        if (!slp_select_model_unpack(f->payload, f->len, &m)) return;
        state_frame(r);
        const bool have = m.kind == SLP_MODEL_UPLOADED && r->stored_valid && r->stored_hash == m.hash;
        if (r->cfg.on_select_model) r->cfg.on_select_model(ctx, &m, have ? r->cfg.model_buf : NULL);
        return;
    }
    case SLP_SNAPSHOT_BEGIN: {
        slp_snapshot_t s;
        if (!slp_snapshot_unpack(f->payload, f->len, &s)) return;
        r->in_snapshot = true;
        r->snap_id = s.snapshot_id;
        r->snap_frames = 0;
        return;
    }
    case SLP_SNAPSHOT_END: {
        slp_snapshot_t s;
        if (!slp_snapshot_unpack(f->payload, f->len, &s)) return;
        // Complete only if BEGIN was seen and every state frame between them arrived.
        if (r->in_snapshot && s.snapshot_id == r->snap_id && s.reason == r->snap_frames) r->applied_snapshot = s.snapshot_id;
        r->in_snapshot = false;
        return;
    }
    case SLP_MODEL_BEGIN: {
        slp_model_begin_t b;
        if (!slp_model_begin_unpack(f->payload, f->len, &b)) return;
        if (!r->cfg.model_buf || b.byte_count > r->cfg.model_buf_size) {
            ack(r, f, SLP_ERR_NO_MEMORY);
            return;
        }
        // The staging buffer is about to be overwritten: it no longer holds a stored model.
        r->stored_valid = false;
        r->up_active = true;
        r->up_transfer = b.transfer_id;
        r->up_size = b.byte_count;
        r->up_crc = b.crc32;
        ack(r, f, SLP_OK);
        return;
    }
    case SLP_MODEL_CHUNK: {
        slp_model_chunk_t c;
        if (!slp_model_chunk_unpack(f->payload, f->len, &c)) return;
        if (!r->up_active || c.transfer_id != r->up_transfer) {
            ack(r, f, SLP_ERR_STATE);
            return;
        }
        if (c.offset > r->up_size || c.size > r->up_size - c.offset) {
            ack(r, f, SLP_ERR_RANGE);
            return;
        }
        memcpy(r->cfg.model_buf + c.offset, c.data, c.size); // idempotent: resends are harmless
        ack(r, f, SLP_OK);
        return;
    }
    case SLP_MODEL_COMMIT:
    case SLP_MODEL_ABORT: {
        slp_model_end_t e;
        if (!slp_model_end_unpack(f->payload, f->len, &e)) return;
        const bool ours = r->up_active && e.transfer_id == r->up_transfer;
        r->up_active = false;
        if (f->type == SLP_MODEL_ABORT) return;
        slp_model_end_t res = { e.transfer_id, SLP_ERR_STATE, 0 };
        if (ours) {
            res.crc32 = slp_crc32(0, r->cfg.model_buf, r->up_size);
            res.status = res.crc32 == r->up_crc ? SLP_OK : SLP_ERR_CRC;
            r->stored_valid = res.status == SLP_OK;
            r->stored_hash = res.crc32;
        }
        uint8_t p[8];
        send(r, SLP_MODEL_RESULT, p, slp_model_end_pack(&res, p, sizeof(p)));
        return;
    }
    default:
        return; // HEARTBEAT needs nothing beyond the timestamp; unknown types are ignored
    }
}

void slp_rx_feed(slp_receiver_t *r, const uint8_t *data, size_t len) {
    slp_frame_t f;
    for (size_t i = 0; i < len; i++) {
        if (!slp_decoder_push(&r->dec, data[i], &f)) continue;
        r->last_rx_ms = r->cfg.now_ms(r->cfg.ctx);
        // Any valid frame from a known ESP32 boot means the link is back.
        if (f.type != SLP_HELLO && r->peer_known) set_link(r, true);
        handle(r, &f);
    }
}

void slp_rx_poll(slp_receiver_t *r) {
    const uint32_t now = r->cfg.now_ms(r->cfg.ctx);
    // Link loss: keep the current sound (never jump to defaults), announce ourselves again.
    if (r->peer_heard && now - r->last_rx_ms > LINK_TIMEOUT_MS) set_link(r, false);
    if (!r->peer_heard && (int32_t)(now - r->next_hello_ms) >= 0) {
        send_hello(r);
        r->next_hello_ms = now + HELLO_PERIOD_MS;
    }
    if ((int32_t)(now - r->next_heartbeat_ms) >= 0) {
        const slp_heartbeat_t hb = { now, r->dec.frames_ok, slp_decoder_errors(&r->dec) };
        uint8_t p[12];
        send(r, SLP_HEARTBEAT, p, slp_heartbeat_pack(&hb, p, sizeof(p)));
        r->next_heartbeat_ms = now + HEARTBEAT_PERIOD_MS;
    }
}

void slp_rx_send_status(slp_receiver_t *r, const slp_status_t *st) {
    slp_status_t s = *st;
    s.applied_snapshot_id = r->applied_snapshot;
    uint8_t p[32];
    send(r, SLP_STATUS, p, slp_status_pack(&s, p, sizeof(p)));
}

void slp_rx_send_meters(slp_receiver_t *r, const slp_meters_t *m) {
    uint8_t p[8];
    send(r, SLP_METERS, p, slp_meters_pack(m, p, sizeof(p)));
}

void slp_rx_send_log(slp_receiver_t *r, const char *text) {
    size_t n = strlen(text);
    if (n > SLP_MAX_PAYLOAD) n = SLP_MAX_PAYLOAD;
    send(r, SLP_LOG, (const uint8_t *)text, n);
}
