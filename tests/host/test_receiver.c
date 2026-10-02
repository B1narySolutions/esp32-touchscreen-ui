// The Seed-side receiver (slp_receiver.c) driven by frames from the real encoder: handshake,
// state hooks, snapshot confirmation, model upload with CRC check, link loss, and noise.
#include <stdlib.h>
#include <string.h>
#include "slp_receiver.h"
#include "test.h"

// --- a fake Seed: clock, captured output, and hook records ---
static uint32_t s_now;
static slp_decoder_t s_out_dec;
#define RING 32
static slp_frame_t s_out[RING];                 // the most recent frames the receiver sent
static uint8_t s_out_payload[RING][SLP_MAX_PAYLOAD];
static int s_out_n;                              // total frames sent
static int s_type_count[256];
static int16_t s_params[0x1000];
static uint8_t s_volume, s_chain_len, s_fx_seen;
static bool s_muted, s_link;
static const uint8_t *s_selected_weights;
static slp_select_model_t s_selected;
static int s_select_calls;

static uint32_t now_ms(void *ctx) { (void)ctx; return s_now; }
static void out(void *ctx, const uint8_t *b, size_t n) {
    (void)ctx;
    slp_frame_t f;
    for (size_t i = 0; i < n; i++) {
        if (slp_decoder_push(&s_out_dec, b[i], &f)) {
            const int k = s_out_n++ % RING;
            memcpy(s_out_payload[k], f.payload, f.len);
            f.payload = s_out_payload[k];
            s_out[k] = f;
            s_type_count[f.type]++;
        }
    }
}
static void on_param(void *c, uint16_t id, int16_t v) { (void)c; if (id < 0x1000) s_params[id] = v; }
static void on_fx(void *c, uint8_t fx, bool on, uint8_t m) { (void)c; (void)on; (void)m; s_fx_seen |= fx == SLP_FX_AMP; }
static void on_chain(void *c, const uint8_t *fx, uint8_t len) { (void)c; (void)fx; s_chain_len = len; }
static void on_master(void *c, uint8_t v, bool m) { (void)c; s_volume = v; s_muted = m; }
static void on_select(void *c, const slp_select_model_t *m, const uint8_t *w) {
    (void)c;
    s_selected = *m;
    s_selected_weights = w;
    s_select_calls++;
}
static void on_link(void *c, bool up) { (void)c; s_link = up; }

static int count_out(uint8_t type) { return s_type_count[type]; }
static const slp_frame_t *last_out(uint8_t type) {
    static slp_frame_t none; // a zero-length frame: unpack fails, the CHECK reports it
    for (int i = s_out_n - 1; i >= 0 && i >= s_out_n - RING; i--) {
        if (s_out[i % RING].type == type) return &s_out[i % RING];
    }
    return &none;
}

// --- the ESP32 side: encode frames into the receiver ---
static uint8_t s_esp_seq;
static uint8_t esp_frame(slp_receiver_t *r, uint8_t type, const uint8_t *p, size_t len, uint8_t flags) {
    uint8_t wire[SLP_MAX_ENCODED];
    const uint8_t seq = s_esp_seq++;
    size_t n = slp_frame_encode(type, seq, flags, p, (uint16_t)len, wire, sizeof(wire));
    slp_rx_feed(r, wire, n);
    return seq;
}

void test_receiver(void) {
    static uint8_t model_buf[7484];
    static const slp_builtin_t builtins[3] = { { 1, "Fender Twin65" }, { 2, "Vox AC30 Chimey" }, { 3, "Marshall JCM800 G5" } };
    slp_rx_config_t cfg = {
        .send = out, .now_ms = now_ms, .boot_id = 0x5EED0001, .fw_version = "test", .sample_rate_hz = 48000,
        .block_size = 48, .builtins = builtins, .builtin_count = 3, .model_buf = model_buf,
        .model_buf_size = sizeof(model_buf), .on_param = on_param, .on_fx = on_fx, .on_chain = on_chain,
        .on_master = on_master, .on_select_model = on_select, .on_link = on_link,
    };
    static slp_receiver_t r; // ~1 KB of decoder buffers: not on the stack
    slp_decoder_init(&s_out_dec);
    slp_rx_init(&r, &cfg);
    uint8_t p[SLP_MAX_PAYLOAD];

    // 1. HELLO every 500 ms until the ESP32 is heard; HEARTBEAT at 1 Hz.
    for (s_now = 0; s_now <= 1600; s_now += 10) slp_rx_poll(&r);
    CHECK_EQ(count_out(SLP_HELLO), 4); // t = 0, 500, 1000, 1500
    CHECK_EQ(count_out(SLP_HEARTBEAT), 2);
    slp_hello_t h;
    CHECK(slp_hello_unpack(last_out(SLP_HELLO)->payload, last_out(SLP_HELLO)->len, &h));
    CHECK(h.role == SLP_ROLE_SEED && h.builtin_count == 3 && strcmp(h.builtins[2].name, "Marshall JCM800 G5") == 0);

    // 2. The ESP32's HELLO: answered once, link up, no more periodic HELLOs.
    slp_hello_t ui = { .proto_version = 1, .role = SLP_ROLE_UI, .boot_id = 0xE5320001 };
    int hellos = count_out(SLP_HELLO);
    esp_frame(&r, SLP_HELLO, p, slp_hello_pack(&ui, p, sizeof(p)), 0);
    CHECK_EQ(count_out(SLP_HELLO), hellos + 1);
    CHECK(s_link && slp_rx_link_up(&r));
    esp_frame(&r, SLP_HELLO, p, slp_hello_pack(&ui, p, sizeof(p)), 0); // same boot: no answer
    CHECK_EQ(count_out(SLP_HELLO), hellos + 1);
    for (; s_now <= 2500; s_now += 10) slp_rx_poll(&r);
    CHECK_EQ(count_out(SLP_HELLO), hellos + 1);

    // 3. A snapshot: every hook fires and STATUS reports it applied.
    slp_snapshot_t sb = { 7, SLP_SNAP_HELLO };
    esp_frame(&r, SLP_SNAPSHOT_BEGIN, p, slp_snapshot_pack(&sb, p, sizeof(p)), 0);
    slp_set_chain_t ch = { 3, { SLP_FX_GATE, SLP_FX_DRIVE, SLP_FX_AMP } };
    esp_frame(&r, SLP_SET_CHAIN, p, slp_set_chain_pack(&ch, p, sizeof(p)), 0);
    slp_set_fx_state_t fs = { 1, { { SLP_FX_AMP, 1, 0 } } };
    esp_frame(&r, SLP_SET_FX_STATE, p, slp_set_fx_state_pack(&fs, p, sizeof(p)), 0);
    slp_select_model_t sm = { SLP_MODEL_BUILTIN, 3, 0 };
    esp_frame(&r, SLP_SELECT_MODEL, p, slp_select_model_pack(&sm, p, sizeof(p)), 0);
    slp_set_master_t ms = { 63, 0 };
    esp_frame(&r, SLP_SET_MASTER, p, slp_set_master_pack(&ms, p, sizeof(p)), 0);
    slp_set_params_t sp = { 2, { { 0x0500, 58 }, { 0x0C02, 55 } } };
    esp_frame(&r, SLP_SET_PARAMS, p, slp_set_params_pack(&sp, p, sizeof(p)), 0);
    slp_snapshot_t se = { 7, 5 };
    esp_frame(&r, SLP_SNAPSHOT_END, p, slp_snapshot_pack(&se, p, sizeof(p)), 0);
    CHECK(s_chain_len == 3 && s_fx_seen && s_volume == 63 && !s_muted);
    CHECK(s_params[0x0500] == 58 && s_params[0x0C02] == 55);
    CHECK(s_selected.kind == SLP_MODEL_BUILTIN && s_selected.builtin_id == 3 && s_selected_weights == NULL);
    slp_status_t st = { 553, 562, 0, SLP_MODEL_BUILTIN, 3, 0, SLP_STATUS_AUDIO_RUNNING, 0 };
    slp_rx_send_status(&r, &st);
    slp_status_t got;
    CHECK(slp_status_unpack(last_out(SLP_STATUS)->payload, last_out(SLP_STATUS)->len, &got));
    CHECK_EQ(got.applied_snapshot_id, 7);

    // An incomplete snapshot (a frame lost) is not confirmed.
    sb.snapshot_id = 8;
    esp_frame(&r, SLP_SNAPSHOT_BEGIN, p, slp_snapshot_pack(&sb, p, sizeof(p)), 0);
    esp_frame(&r, SLP_SET_MASTER, p, slp_set_master_pack(&ms, p, sizeof(p)), 0);
    se.snapshot_id = 8;
    esp_frame(&r, SLP_SNAPSHOT_END, p, slp_snapshot_pack(&se, p, sizeof(p)), 0); // says 5 frames, 1 arrived
    slp_rx_send_status(&r, &st);
    CHECK(slp_status_unpack(last_out(SLP_STATUS)->payload, last_out(SLP_STATUS)->len, &got));
    CHECK_EQ(got.applied_snapshot_id, 7);

    // Mute applies at once.
    ms.muted = 1;
    esp_frame(&r, SLP_SET_MASTER, p, slp_set_master_pack(&ms, p, sizeof(p)), 0);
    CHECK(s_muted && s_volume == 63);

    // 4. PING -> PONG with the same payload.
    slp_ping_t pg = { 42, 1234 };
    esp_frame(&r, SLP_PING, p, slp_ping_pack(&pg, p, sizeof(p)), 0);
    slp_ping_t pong;
    CHECK(slp_ping_unpack(last_out(SLP_PONG)->payload, last_out(SLP_PONG)->len, &pong) && pong.token == 42);

    // 5. Model upload: chunks in any order (and one resent), CRC verified, selectable after.
    static uint8_t weights[7484];
    unsigned seed = 77;
    for (size_t i = 0; i < sizeof(weights); i++) weights[i] = (uint8_t)test_rand(&seed);
    const uint32_t crc = slp_crc32(0, weights, sizeof(weights));
    slp_model_begin_t mb = { 1, 1871, 7484, crc, "Test amp" };
    uint8_t bseq = esp_frame(&r, SLP_MODEL_BEGIN, p, slp_model_begin_pack(&mb, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    slp_ack_t a;
    CHECK(slp_ack_unpack(last_out(SLP_ACK)->payload, 3, &a) && a.acked_seq == bseq && a.status == SLP_OK);
    for (int pass = 1; pass >= 0; pass--) { // odd chunks first, then even: order doesn't matter
        for (uint32_t off = (uint32_t)pass * SLP_CHUNK_MAX; off < sizeof(weights); off += 2 * SLP_CHUNK_MAX) {
            uint32_t size = sizeof(weights) - off < SLP_CHUNK_MAX ? (uint32_t)sizeof(weights) - off : SLP_CHUNK_MAX;
            slp_model_chunk_t c = { 1, off, (uint8_t)size, weights + off };
            uint8_t cseq = esp_frame(&r, SLP_MODEL_CHUNK, p, slp_model_chunk_pack(&c, p, sizeof(p)), SLP_FLAG_ACK_REQ);
            CHECK(slp_ack_unpack(last_out(SLP_ACK)->payload, 3, &a) && a.acked_seq == cseq && a.status == SLP_OK);
            if (off == SLP_CHUNK_MAX * 4) esp_frame(&r, SLP_MODEL_CHUNK, p, slp_model_chunk_pack(&c, p, sizeof(p)), SLP_FLAG_ACK_REQ);
        }
    }
    slp_model_end_t cm = { 1, 0, 0 };
    esp_frame(&r, SLP_MODEL_COMMIT, p, slp_model_end_pack(&cm, p, sizeof(p)), 0);
    slp_model_end_t res;
    CHECK(slp_model_end_unpack(last_out(SLP_MODEL_RESULT)->payload, 6, &res));
    CHECK(res.status == SLP_OK && res.crc32 == crc);
    slp_select_model_t up = { SLP_MODEL_UPLOADED, 0, crc };
    esp_frame(&r, SLP_SELECT_MODEL, p, slp_select_model_pack(&up, p, sizeof(p)), 0);
    CHECK(s_selected_weights == model_buf && memcmp(model_buf, weights, sizeof(weights)) == 0);
    up.hash = crc ^ 1; // a model the Seed doesn't hold
    esp_frame(&r, SLP_SELECT_MODEL, p, slp_select_model_pack(&up, p, sizeof(p)), 0);
    CHECK(s_selected_weights == NULL);

    // A corrupted upload fails its CRC, an out-of-range chunk is refused, oversize is refused.
    mb.transfer_id = 2;
    esp_frame(&r, SLP_MODEL_BEGIN, p, slp_model_begin_pack(&mb, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    for (uint32_t off = 0; off < sizeof(weights); off += SLP_CHUNK_MAX) {
        uint32_t size = sizeof(weights) - off < SLP_CHUNK_MAX ? (uint32_t)sizeof(weights) - off : SLP_CHUNK_MAX;
        uint8_t chunk[SLP_CHUNK_MAX];
        memcpy(chunk, weights + off, size);
        if (off == 0) chunk[5] ^= 0x40;
        slp_model_chunk_t c = { 2, off, (uint8_t)size, chunk };
        esp_frame(&r, SLP_MODEL_CHUNK, p, slp_model_chunk_pack(&c, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    }
    cm.transfer_id = 2;
    esp_frame(&r, SLP_MODEL_COMMIT, p, slp_model_end_pack(&cm, p, sizeof(p)), 0);
    CHECK(slp_model_end_unpack(last_out(SLP_MODEL_RESULT)->payload, 6, &res) && res.status == SLP_ERR_CRC);
    up.hash = crc;
    esp_frame(&r, SLP_SELECT_MODEL, p, slp_select_model_pack(&up, p, sizeof(p)), 0);
    CHECK(s_selected_weights == NULL); // the good copy was overwritten: the ESP32 must upload again
    mb.transfer_id = 3;
    esp_frame(&r, SLP_MODEL_BEGIN, p, slp_model_begin_pack(&mb, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    slp_model_chunk_t far = { 3, 7400, 100, weights };
    esp_frame(&r, SLP_MODEL_CHUNK, p, slp_model_chunk_pack(&far, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    CHECK(slp_ack_unpack(last_out(SLP_ACK)->payload, 3, &a) && a.status == SLP_ERR_RANGE);
    slp_model_begin_t big = { 4, 4000, 16000, 0, "too big" };
    esp_frame(&r, SLP_MODEL_BEGIN, p, slp_model_begin_pack(&big, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    CHECK(slp_ack_unpack(last_out(SLP_ACK)->payload, 3, &a) && a.status == SLP_ERR_NO_MEMORY);

    // 6. Silence for 3 s: link down (state untouched), HELLOs resume; a frame brings it back.
    const uint8_t vol_before = s_volume;
    hellos = count_out(SLP_HELLO);
    const uint32_t t_quiet = s_now;
    for (s_now += 10; count_out(SLP_HELLO) == hellos && s_now - t_quiet < 10000; s_now += 10) slp_rx_poll(&r);
    CHECK(s_now - t_quiet >= 3000 && s_now - t_quiet < 3600);
    CHECK(!s_link && s_volume == vol_before);
    slp_heartbeat_t hb = { 1, 2, 3 };
    esp_frame(&r, SLP_HEARTBEAT, p, slp_heartbeat_pack(&hb, p, sizeof(p)), 0);
    CHECK(s_link);

    // 7. Noise never crashes it.
    uint8_t junk[4096];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)(test_rand(&seed) % 8 == 0 ? 0 : test_rand(&seed));
    slp_rx_feed(&r, junk, sizeof(junk));
}
