// COBS and CRC vectors, frame round trips, and pack/unpack of every message.
#include <stdlib.h>
#include <string.h>
#include "seed_link_proto.h"
#include "test.h"

static void cobs_vector(const uint8_t *in, size_t n, const uint8_t *want, size_t wn) {
    uint8_t enc[700], dec[700];
    size_t e = slp_cobs_encode(in, n, enc, sizeof(enc));
    CHECK_EQ(e, wn);
    CHECK(e == wn && memcmp(enc, want, wn) == 0);
    size_t d = slp_cobs_decode(enc, e, dec, sizeof(dec));
    CHECK_EQ(d, n);
    CHECK(d == n && memcmp(dec, in, n) == 0);
}

static void test_cobs(void) {
    // Vectors from the COBS paper / Wikipedia.
    { const uint8_t i[] = { 0x00 }, w[] = { 0x01, 0x01 }; cobs_vector(i, 1, w, 2); }
    { const uint8_t i[] = { 0x00, 0x00 }, w[] = { 0x01, 0x01, 0x01 }; cobs_vector(i, 2, w, 3); }
    { const uint8_t i[] = { 0x00, 0x11, 0x00 }, w[] = { 0x01, 0x02, 0x11, 0x01 }; cobs_vector(i, 3, w, 4); }
    { const uint8_t i[] = { 0x11, 0x22, 0x00, 0x33 }, w[] = { 0x03, 0x11, 0x22, 0x02, 0x33 }; cobs_vector(i, 4, w, 5); }
    { const uint8_t i[] = { 0x11, 0x22, 0x33, 0x44 }, w[] = { 0x05, 0x11, 0x22, 0x33, 0x44 }; cobs_vector(i, 4, w, 5); }
    { const uint8_t i[] = { 0x11, 0x00, 0x00, 0x00 }, w[] = { 0x02, 0x11, 0x01, 0x01, 0x01 }; cobs_vector(i, 4, w, 5); }

    // 01..FE (254 bytes) -> FF 01..FE
    uint8_t in[300], want[300];
    for (int k = 0; k < 254; k++) in[k] = (uint8_t)(k + 1);
    want[0] = 0xFF;
    memcpy(want + 1, in, 254);
    cobs_vector(in, 254, want, 255);
    // 00 01..FE (255 bytes) -> 01 FF 01..FE
    in[0] = 0;
    for (int k = 1; k < 255; k++) in[k] = (uint8_t)k;
    want[0] = 0x01; want[1] = 0xFF;
    for (int k = 1; k < 255; k++) want[k + 1] = (uint8_t)k;
    cobs_vector(in, 255, want, 256);
    // 01..FF (255 bytes) -> FF 01..FE 02 FF
    for (int k = 0; k < 255; k++) in[k] = (uint8_t)(k + 1);
    want[0] = 0xFF;
    for (int k = 0; k < 254; k++) want[k + 1] = (uint8_t)(k + 1);
    want[255] = 0x02; want[256] = 0xFF;
    cobs_vector(in, 255, want, 257);

    // Empty input encodes to a single 0x01.
    { const uint8_t w[] = { 0x01 }; cobs_vector(in, 0, w, 1); }

    // Random round trips, 0..600 bytes, encoded output never contains 0x00.
    unsigned seed = 1;
    for (int t = 0; t < 2000; t++) {
        uint8_t src[600], enc[620], dec[600];
        size_t n = test_rand(&seed) % 600;
        for (size_t k = 0; k < n; k++) src[k] = (test_rand(&seed) % 4 == 0) ? 0 : (uint8_t)test_rand(&seed);
        size_t e = slp_cobs_encode(src, n, enc, sizeof(enc));
        CHECK(e > 0 && e <= n + n / 254 + 1);
        CHECK(memchr(enc, 0, e) == NULL);
        CHECK_EQ(slp_cobs_decode(enc, e, dec, sizeof(dec)), n);
        CHECK(memcmp(src, dec, n) == 0);
    }

    // Output buffer too small is an error, never an overflow.
    uint8_t small[4];
    CHECK_EQ(slp_cobs_encode(in, 10, small, sizeof(small)), 0);
    const uint8_t enc5[] = { 0x05, 0x11, 0x22, 0x33, 0x44 };
    CHECK_EQ(slp_cobs_decode(enc5, 5, small, 3), 0);
    // A code byte promising more data than there is.
    const uint8_t trunc[] = { 0x05, 0x11 };
    CHECK_EQ(slp_cobs_decode(trunc, 2, small, sizeof(small)), 0);
}

static void test_crc(void) {
    const uint8_t check[] = "123456789";
    CHECK_EQ(slp_crc16(check, 9), 0x29B1);        // CRC-16/CCITT-FALSE check value
    CHECK_EQ(slp_crc32(0, check, 9), 0xCBF43926u); // CRC-32 check value
    // Chained CRC32 equals one-shot.
    CHECK_EQ(slp_crc32(slp_crc32(0, check, 4), check + 4, 5), 0xCBF43926u);
    CHECK_EQ(slp_crc16(check, 0), 0xFFFF);
}

static void test_frames(void) {
    uint8_t payload[SLP_MAX_PAYLOAD], wire[SLP_MAX_ENCODED * 3];
    for (int k = 0; k < SLP_MAX_PAYLOAD; k++) payload[k] = (uint8_t)(k * 7);

    // Every payload length round-trips with its header fields.
    slp_decoder_t d;
    slp_decoder_init(&d);
    for (int len = 0; len <= SLP_MAX_PAYLOAD; len++) {
        size_t n = slp_frame_encode(0x42, (uint8_t)len, 0x01, payload, (uint16_t)len, wire, SLP_MAX_ENCODED);
        CHECK(n > 0 && n <= SLP_MAX_ENCODED && wire[0] == 0 && wire[n - 1] == 0);
        CHECK(memchr(wire + 1, 0, n - 2) == NULL);
        int got = 0;
        slp_frame_t f;
        for (size_t k = 0; k < n; k++) {
            if (slp_decoder_push(&d, wire[k], &f)) {
                got++;
                CHECK_EQ(f.type, 0x42);
                CHECK_EQ(f.seq, (uint8_t)len);
                CHECK_EQ(f.flags, 0x01);
                CHECK_EQ(f.len, len);
                CHECK(memcmp(f.payload, payload, (size_t)len) == 0);
            }
        }
        CHECK_EQ(got, 1);
    }
    CHECK_EQ(slp_decoder_errors(&d), 0);

    // Too long a payload, or too small an output buffer, is refused.
    CHECK_EQ(slp_frame_encode(1, 0, 0, payload, SLP_MAX_PAYLOAD + 1, wire, sizeof(wire)), 0);
    CHECK_EQ(slp_frame_encode(1, 0, 0, payload, 100, wire, 50), 0);

    // Garbage before a frame (the ESP32 boot banner) costs at most one dropped frame.
    slp_decoder_init(&d);
    const char banner[] = "ESP-ROM:esp32s3-20210327\r\nBuild:Mar 27 2021\r\nrst:0x1 (POWERON)\r\n";
    slp_frame_t f;
    int got = 0;
    for (size_t k = 0; k < sizeof(banner) - 1; k++) got += slp_decoder_push(&d, (uint8_t)banner[k], &f);
    size_t n = slp_frame_encode(SLP_HEARTBEAT, 7, 0, payload, 12, wire, sizeof(wire));
    for (size_t k = 0; k < n; k++) got += slp_decoder_push(&d, wire[k], &f);
    CHECK_EQ(got, 1);
    CHECK_EQ(f.type, SLP_HEARTBEAT);
    CHECK(slp_decoder_errors(&d) <= 1);

    // A flipped bit is caught by the CRC, and the next frame still decodes.
    slp_decoder_init(&d);
    n = slp_frame_encode(SLP_PING, 1, 0, payload, 8, wire, sizeof(wire));
    wire[3] ^= 0x04;
    got = 0;
    for (size_t k = 0; k < n; k++) got += slp_decoder_push(&d, wire[k], &f);
    n = slp_frame_encode(SLP_PING, 2, 0, payload, 8, wire, sizeof(wire));
    for (size_t k = 0; k < n; k++) got += slp_decoder_push(&d, wire[k], &f);
    CHECK_EQ(got, 1);
    CHECK_EQ(f.seq, 2);
    CHECK_EQ(slp_decoder_errors(&d), 1);

    // A wrong protocol version is counted and dropped.
    {
        uint8_t raw[SLP_MAX_FRAME] = { 2, SLP_PING, 0, 0, 0, 0 };
        uint16_t crc = slp_crc16(raw, 6);
        raw[6] = (uint8_t)crc;
        raw[7] = (uint8_t)(crc >> 8);
        uint8_t enc[16];
        size_t e = slp_cobs_encode(raw, 8, enc, sizeof(enc));
        slp_decoder_init(&d);
        got = 0;
        for (size_t k = 0; k < e; k++) got += slp_decoder_push(&d, enc[k], &f);
        got += slp_decoder_push(&d, 0, &f);
        CHECK_EQ(got, 0);
        CHECK_EQ(d.err_version, 1);
    }

    // An endless run of non-zero bytes overflows once, then the decoder recovers.
    slp_decoder_init(&d);
    for (int k = 0; k < 5000; k++) slp_decoder_push(&d, 0x55, &f);
    slp_decoder_push(&d, 0, &f);
    CHECK_EQ(d.err_overflow, 1);
    n = slp_frame_encode(SLP_PING, 3, 0, payload, 8, wire, sizeof(wire));
    got = 0;
    for (size_t k = 0; k < n; k++) got += slp_decoder_push(&d, wire[k], &f);
    CHECK_EQ(got, 1);
}

// Every message: pack, unpack, compare, and reject every wrong length.
#define ROUND_TRIP(T, msg, cmp_expr)                                                     \
    do {                                                                                  \
        uint8_t buf_[SLP_MAX_PAYLOAD];                                                    \
        size_t n_ = slp_##T##_pack(&(msg), buf_, sizeof(buf_));                           \
        CHECK(n_ > 0);                                                                    \
        slp_##T##_t out_;                                                                 \
        memset(&out_, 0xA5, sizeof(out_));                                                \
        CHECK(slp_##T##_unpack(buf_, n_, &out_));                                         \
        { slp_##T##_t *a = &(msg), *b = &out_; (void)a; (void)b; CHECK(cmp_expr); }       \
        for (size_t l_ = 0; l_ < n_ + 3 && l_ < sizeof(buf_); l_++) {                     \
            if (l_ == n_) continue;                                                       \
            uint8_t *copy_ = malloc(l_ ? l_ : 1); /* exact size: ASan sees over-reads */ \
            memcpy(copy_, buf_, l_);                                                      \
            CHECK(!slp_##T##_unpack(copy_, l_, &out_));                                   \
            free(copy_);                                                                  \
        }                                                                                 \
        CHECK_EQ(slp_##T##_pack(&(msg), buf_, n_ - 1), 0);                                \
    } while (0)

static void test_messages(void) {
    slp_hello_t h = { .proto_version = 1, .role = SLP_ROLE_SEED, .boot_id = 0xDEADBEEF,
                      .fw_version = "nam-0.3.1", .sample_rate_hz = 48000, .block_size = 48,
                      .builtin_count = 3,
                      .builtins = { { 1, "Fender Twin65" }, { 2, "Vox AC30 Chimey" }, { 3, "Marshall JCM800 G5" } } };
    ROUND_TRIP(hello, h, a->boot_id == b->boot_id && a->role == b->role && b->builtin_count == 3 &&
               strcmp(b->fw_version, "nam-0.3.1") == 0 && strcmp(b->builtins[2].name, "Marshall JCM800 G5") == 0 &&
               b->sample_rate_hz == 48000 && b->block_size == 48 && b->builtins[1].id == 2);
    // Names exactly as long as the field come back terminated.
    slp_hello_t full = h;
    memset(full.fw_version, 'v', SLP_FW_LEN);
    full.fw_version[SLP_FW_LEN] = 0;
    memset(full.builtins[0].name, 'n', SLP_NAME_LEN);
    full.builtins[0].name[SLP_NAME_LEN] = 0;
    ROUND_TRIP(hello, full, strlen(b->fw_version) == SLP_FW_LEN && strlen(b->builtins[0].name) == SLP_NAME_LEN);
    slp_hello_t empty = { .proto_version = 1, .role = SLP_ROLE_UI, .boot_id = 7, .fw_version = "abc" };
    ROUND_TRIP(hello, empty, b->builtin_count == 0 && b->role == SLP_ROLE_UI);
    slp_hello_t bad = h;
    bad.builtin_count = SLP_MAX_BUILTINS + 1;
    uint8_t buf[SLP_MAX_PAYLOAD];
    CHECK_EQ(slp_hello_pack(&bad, buf, sizeof(buf)), 0);

    slp_heartbeat_t hb = { 123456, 789, 3 };
    ROUND_TRIP(heartbeat, hb, memcmp(a, b, sizeof(*a)) == 0);
    slp_ping_t pg = { 0x01020304, 0xA0B0C0D0 };
    ROUND_TRIP(ping, pg, memcmp(a, b, sizeof(*a)) == 0);
    slp_ack_t ack = { SLP_MODEL_CHUNK, 200, SLP_ERR_RANGE };
    ROUND_TRIP(ack, ack, a->acked_type == b->acked_type && a->acked_seq == b->acked_seq && a->status == b->status);

    slp_set_params_t sp = { .count = 3, .params = { { 0x0500, 58 }, { 0x0C02, 0 }, { 0x0101, -1 } } };
    ROUND_TRIP(set_params, sp, b->count == 3 && b->params[0].id == 0x0500 && b->params[0].value == 58 &&
               b->params[2].value == -1);
    slp_set_params_t sp_max = { .count = SLP_MAX_PARAMS };
    for (int k = 0; k < SLP_MAX_PARAMS; k++) sp_max.params[k] = (slp_param_t){ (uint16_t)(0x100 + k), (int16_t)k };
    ROUND_TRIP(set_params, sp_max, b->count == SLP_MAX_PARAMS && b->params[62].id == 0x100 + 62);
    sp_max.count = 0;
    CHECK_EQ(slp_set_params_pack(&sp_max, buf, sizeof(buf)), 0);

    slp_set_fx_state_t fs = { .count = 2, .fx = { { SLP_FX_AMP, 1, 2 }, { SLP_FX_CAB, 1, 0 } } };
    ROUND_TRIP(set_fx_state, fs, b->count == 2 && b->fx[0].fx == SLP_FX_AMP && b->fx[0].model == 2);
    fs.fx[1].fx = 13;
    CHECK_EQ(slp_set_fx_state_pack(&fs, buf, sizeof(buf)), 0);

    slp_set_chain_t ch = { .len = 7, .fx = { 1, 2, 4, 5, 7, 10, 11 } };
    ROUND_TRIP(set_chain, ch, b->len == 7 && memcmp(a->fx, b->fx, 7) == 0);
    slp_set_chain_t dup = { .len = 2, .fx = { 4, 4 } };
    CHECK_EQ(slp_set_chain_pack(&dup, buf, sizeof(buf)), 0);
    slp_set_chain_t cab = { .len = 1, .fx = { SLP_FX_CAB } };
    CHECK_EQ(slp_set_chain_pack(&cab, buf, sizeof(buf)), 0);
    { const uint8_t raw[] = { 2, 4, 4 }; slp_set_chain_t o; CHECK(!slp_set_chain_unpack(raw, 3, &o)); }

    slp_set_master_t ms = { 63, 1 };
    ROUND_TRIP(set_master, ms, b->volume == 63 && b->muted == 1);
    { const uint8_t raw[] = { 101, 0 }; slp_set_master_t o; CHECK(!slp_set_master_unpack(raw, 2, &o)); }
    { const uint8_t raw[] = { 50, 2 }; slp_set_master_t o; CHECK(!slp_set_master_unpack(raw, 2, &o)); }

    slp_select_model_t sm = { SLP_MODEL_UPLOADED, 0, 0x89ABCDEF };
    ROUND_TRIP(select_model, sm, b->kind == SLP_MODEL_UPLOADED && b->hash == 0x89ABCDEF);
    { const uint8_t raw[] = { 9, 0, 0, 0, 0, 0 }; slp_select_model_t o; CHECK(!slp_select_model_unpack(raw, 6, &o)); }

    slp_snapshot_t ss = { 0xBEEF, SLP_SNAP_PRESET };
    ROUND_TRIP(snapshot, ss, b->snapshot_id == 0xBEEF && b->reason == SLP_SNAP_PRESET);

    slp_status_t st = { 553, 562, 4, SLP_MODEL_BUILTIN, 3, 0, SLP_STATUS_AUDIO_RUNNING, 0x1234 };
    ROUND_TRIP(status, st, b->cpu_avg_x10 == 553 && b->overruns == 4 && b->builtin_id == 3 &&
               b->applied_snapshot_id == 0x1234 && b->flags == SLP_STATUS_AUDIO_RUNNING);

    slp_meters_t me = { -600, SLP_METER_SILENCE, 17 };
    ROUND_TRIP(meters, me, b->in_peak_cdb == -600 && b->out_peak_cdb == SLP_METER_SILENCE && b->clip_count == 17);

    slp_model_begin_t mb = { 9, 1871, 7484, 0xCAFEBABE, "TONE3000 Plexi A2" };
    ROUND_TRIP(model_begin, mb, b->weight_count == 1871 && b->byte_count == 7484 && b->crc32 == 0xCAFEBABE &&
               strcmp(b->name, "TONE3000 Plexi A2") == 0);
    mb.byte_count = 7000;
    CHECK_EQ(slp_model_begin_pack(&mb, buf, sizeof(buf)), 0);

    uint8_t data[SLP_CHUNK_MAX];
    for (int k = 0; k < SLP_CHUNK_MAX; k++) data[k] = (uint8_t)(255 - k);
    slp_model_chunk_t mc = { 9, 7296, SLP_CHUNK_MAX, data };
    {
        size_t n = slp_model_chunk_pack(&mc, buf, sizeof(buf));
        CHECK_EQ(n, 5 + SLP_CHUNK_MAX);
        slp_model_chunk_t o;
        CHECK(slp_model_chunk_unpack(buf, n, &o));
        CHECK(o.offset == 7296 && o.size == SLP_CHUNK_MAX && memcmp(o.data, data, SLP_CHUNK_MAX) == 0);
        CHECK(!slp_model_chunk_unpack(buf, 5, &o));                    // no data
        CHECK(!slp_model_chunk_unpack(buf, 5 + SLP_CHUNK_MAX + 1, &o)); // too much data
        mc.size = 0;
        CHECK_EQ(slp_model_chunk_pack(&mc, buf, sizeof(buf)), 0);
    }

    slp_model_end_t me2 = { 9, SLP_ERR_CRC, 0x11223344 };
    ROUND_TRIP(model_end, me2, b->transfer_id == 9 && b->status == SLP_ERR_CRC && b->crc32 == 0x11223344);
}

void test_proto(void) {
    test_cobs();
    test_crc();
    test_frames();
    test_messages();
}
