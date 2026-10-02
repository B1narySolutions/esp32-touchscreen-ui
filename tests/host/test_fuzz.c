// Decoder fuzzer: random bytes, mutated and truncated valid frames, and random payloads for
// every unpacker. Nothing may crash or read out of bounds (built with AddressSanitizer), and
// valid frames interleaved with junk must still come through intact.
#include <stdlib.h>
#include <string.h>
#include "seed_link_proto.h"
#include "test.h"

typedef bool (*unpack_fn)(const uint8_t *p, size_t len, void *out);

static void unpack_any(uint8_t type, const uint8_t *p, size_t len) {
    union {
        slp_hello_t hello; slp_heartbeat_t hb; slp_ping_t ping; slp_ack_t ack; slp_set_params_t sp;
        slp_set_fx_state_t fs; slp_set_chain_t ch; slp_set_master_t ms; slp_select_model_t sm;
        slp_snapshot_t ss; slp_status_t st; slp_meters_t me; slp_model_begin_t mb;
        slp_model_chunk_t mc; slp_model_end_t mend;
    } u;
    switch (type) {
    case SLP_HELLO: slp_hello_unpack(p, len, &u.hello); break;
    case SLP_HEARTBEAT: slp_heartbeat_unpack(p, len, &u.hb); break;
    case SLP_PING: case SLP_PONG: slp_ping_unpack(p, len, &u.ping); break;
    case SLP_ACK: slp_ack_unpack(p, len, &u.ack); break;
    case SLP_SET_PARAMS: slp_set_params_unpack(p, len, &u.sp); break;
    case SLP_SET_FX_STATE: slp_set_fx_state_unpack(p, len, &u.fs); break;
    case SLP_SET_CHAIN: slp_set_chain_unpack(p, len, &u.ch); break;
    case SLP_SET_MASTER: slp_set_master_unpack(p, len, &u.ms); break;
    case SLP_SELECT_MODEL: slp_select_model_unpack(p, len, &u.sm); break;
    case SLP_SNAPSHOT_BEGIN: case SLP_SNAPSHOT_END: slp_snapshot_unpack(p, len, &u.ss); break;
    case SLP_STATUS: slp_status_unpack(p, len, &u.st); break;
    case SLP_METERS: slp_meters_unpack(p, len, &u.me); break;
    case SLP_MODEL_BEGIN: slp_model_begin_unpack(p, len, &u.mb); break;
    case SLP_MODEL_CHUNK:
        if (slp_model_chunk_unpack(p, len, &u.mc)) {
            // Touch every data byte the unpacker claims is there.
            volatile uint8_t sum = 0;
            for (int k = 0; k < u.mc.size; k++) sum += u.mc.data[k];
            (void)sum;
        }
        break;
    case SLP_MODEL_COMMIT: case SLP_MODEL_RESULT: case SLP_MODEL_ABORT: slp_model_end_unpack(p, len, &u.mend); break;
    default: break;
    }
}

static const uint8_t k_types[] = {
    SLP_HELLO, SLP_HEARTBEAT, SLP_PING, SLP_PONG, SLP_ACK, SLP_LOG, SLP_SET_PARAMS, SLP_SET_FX_STATE,
    SLP_SET_CHAIN, SLP_SET_MASTER, SLP_SELECT_MODEL, SLP_SNAPSHOT_BEGIN, SLP_SNAPSHOT_END, SLP_STATUS,
    SLP_METERS, SLP_MODEL_BEGIN, SLP_MODEL_CHUNK, SLP_MODEL_COMMIT, SLP_MODEL_RESULT, SLP_MODEL_ABORT,
};

void test_fuzz(void) {
    unsigned seed = 0x5EED;
    slp_decoder_t *d = malloc(sizeof(*d)); // heap, so ASan guards its edges
    slp_frame_t f;

    // 1. Pure noise, with zeros at a realistic rate: frames that pass the CRC by chance are
    //    unpacked anyway, and the unpacker must cope.
    slp_decoder_init(d);
    unsigned accepted = 0;
    for (int i = 0; i < 2000000; i++) {
        uint8_t b = (test_rand(&seed) % 64 == 0) ? 0 : (uint8_t)test_rand(&seed);
        if (slp_decoder_push(d, b, &f)) {
            accepted++;
            CHECK(f.len <= SLP_MAX_PAYLOAD);
            uint8_t *copy = malloc(f.len ? f.len : 1);
            memcpy(copy, f.payload, f.len);
            unpack_any(f.type, copy, f.len);
            free(copy);
        }
    }
    // A 16-bit CRC lets about 1 in 65536 random frames through; anything far above that is a bug.
    CHECK(accepted < 50);

    // 2. Valid frames, each with a random mutation (bit flip, dropped byte, inserted byte,
    //    truncation), interleaved with clean frames that must all arrive.
    slp_decoder_init(d);
    unsigned clean_sent = 0, clean_got = 0;
    for (int i = 0; i < 20000; i++) {
        uint8_t payload[SLP_MAX_PAYLOAD], wire[SLP_MAX_ENCODED + 8];
        uint16_t len = (uint16_t)(test_rand(&seed) % (SLP_MAX_PAYLOAD + 1));
        for (int k = 0; k < len; k++) payload[k] = (uint8_t)test_rand(&seed);
        const uint8_t type = k_types[test_rand(&seed) % sizeof(k_types)];
        size_t n = slp_frame_encode(type, (uint8_t)i, 0, payload, len, wire, SLP_MAX_ENCODED);
        CHECK(n > 0);

        const bool mutate = (i % 2) == 0;
        if (mutate) {
            switch (test_rand(&seed) % 4) {
            case 0: wire[test_rand(&seed) % (n - 1)] ^= (uint8_t)(1u << (test_rand(&seed) % 8)); break;
            case 1: { size_t at = test_rand(&seed) % (n - 1); memmove(wire + at, wire + at + 1, n - at - 1); n--; break; }
            case 2: { size_t at = test_rand(&seed) % (n - 1); memmove(wire + at + 1, wire + at, n - at); wire[at] = (uint8_t)test_rand(&seed); n++; break; }
            case 3: n = 1 + test_rand(&seed) % (n - 1); wire[n - 1] = 0; break; // truncated, then a delimiter
            }
        } else {
            clean_sent++;
        }
        for (size_t k = 0; k < n; k++) {
            if (slp_decoder_push(d, wire[k], &f)) {
                if (!mutate && f.seq == (uint8_t)i && f.len == len && memcmp(f.payload, payload, len) == 0) clean_got++;
                uint8_t *copy = malloc(f.len ? f.len : 1);
                memcpy(copy, f.payload, f.len);
                unpack_any(f.type, copy, f.len);
                free(copy);
            }
        }
        // A mutation that removed the delimiter merges into the next frame; end with an extra 0
        // so each clean frame starts fresh, as an idle line would.
        slp_decoder_push(d, 0, &f);
    }
    CHECK_EQ(clean_got, clean_sent);

    // 3. Every unpacker with random payloads of every length, in exact-size heap buffers.
    for (size_t t = 0; t < sizeof(k_types); t++) {
        for (size_t len = 0; len <= SLP_MAX_PAYLOAD; len++) {
            for (int rep = 0; rep < 8; rep++) {
                uint8_t *p = malloc(len ? len : 1);
                for (size_t k = 0; k < len; k++) p[k] = (uint8_t)test_rand(&seed);
                // Make the count bytes plausible half the time so the deeper paths run.
                if (len && rep % 2) p[0] = (uint8_t)(test_rand(&seed) % 70);
                if (len > 28 && rep % 2) p[28] = (uint8_t)(test_rand(&seed) % 10);
                unpack_any(k_types[t], p, len);
                free(p);
            }
        }
    }

    // 4. COBS decode of random non-zero input into a tight buffer never overflows.
    for (int i = 0; i < 20000; i++) {
        size_t n = 1 + test_rand(&seed) % 300, cap = test_rand(&seed) % 300;
        uint8_t *in = malloc(n), *out = malloc(cap ? cap : 1);
        for (size_t k = 0; k < n; k++) in[k] = (uint8_t)(1 + test_rand(&seed) % 255);
        size_t got = slp_cobs_decode(in, n, out, cap);
        CHECK(got <= cap);
        free(in);
        free(out);
    }
    free(d);
}
