// Prints one real encoded frame per message type, in the form docs/SEED_LINK_PROTOCOL.md uses.
// The spec's hex examples are copied from this output, so they always match the code.
#include <stdio.h>
#include <string.h>
#include "seed_link_proto.h"

static void hex(const char *label, const uint8_t *p, size_t n) {
    printf("%-8s", label);
    for (size_t i = 0; i < n; i++) printf("%02X%s", p[i], i + 1 < n ? " " : "");
    printf("\n");
}

static void show(const char *title, uint8_t type, uint8_t seq, uint8_t flags, const uint8_t *payload, size_t len) {
    uint8_t wire[SLP_MAX_ENCODED];
    size_t n = slp_frame_encode(type, seq, flags, payload, (uint16_t)len, wire, sizeof(wire));
    printf("### %s\n```\n", title);
    hex("payload", payload, len);
    hex("wire", wire, n);
    printf("```\n\n");
}

int main(void) {
    uint8_t p[SLP_MAX_PAYLOAD];
    size_t n;

    slp_hello_t seed = { .proto_version = 1, .role = SLP_ROLE_SEED, .boot_id = 0x1A2B3C4D,
                         .fw_version = "nam-a2-1.0", .sample_rate_hz = 48000, .block_size = 48,
                         .builtin_count = 3,
                         .builtins = { { 1, "Fender Twin65" }, { 2, "Vox AC30 Chimey" }, { 3, "Marshall JCM800 G5" } } };
    n = slp_hello_pack(&seed, p, sizeof(p));
    show("HELLO from the Seed (seq 0): role 2, boot_id 0x1A2B3C4D, 48000 Hz, 48-sample blocks, 3 built-in amps",
         SLP_HELLO, 0, 0, p, n);

    slp_hello_t ui = { .proto_version = 1, .role = SLP_ROLE_UI, .boot_id = 0x00C0FFEE, .fw_version = "fbd5d99" };
    n = slp_hello_pack(&ui, p, sizeof(p));
    show("HELLO from the ESP32 (seq 0): role 1, no built-ins", SLP_HELLO, 0, 0, p, n);

    slp_heartbeat_t hb = { 61000, 1234, 2 };
    n = slp_heartbeat_pack(&hb, p, sizeof(p));
    show("HEARTBEAT (seq 17): uptime 61 s, 1234 frames received, 2 dropped", SLP_HEARTBEAT, 17, 0, p, n);

    slp_ping_t ping = { 42, 123456789 };
    n = slp_ping_pack(&ping, p, sizeof(p));
    show("PING (seq 18): token 42, t_send_us 123456789 (PONG echoes the same payload)", SLP_PING, 18, 0, p, n);

    slp_set_params_t sp = { .count = 2, .params = { { 0x0500, 58 }, { 0x0503, 66 } } };
    n = slp_set_params_pack(&sp, p, sizeof(p));
    show("SET_PARAMS (seq 19): amp.gain = 58, amp.treble = 66", SLP_SET_PARAMS, 19, 0, p, n);

    slp_set_fx_state_t fs = { .count = 3, .fx = { { SLP_FX_DRIVE, 1, 0 }, { SLP_FX_AMP, 1, 2 }, { SLP_FX_REVERB, 0, 0 } } };
    n = slp_set_fx_state_pack(&fs, p, sizeof(p));
    show("SET_FX_STATE (seq 20): drive on (model 0), amp on (model 2), reverb bypassed", SLP_SET_FX_STATE, 20, 0, p, n);

    slp_set_chain_t ch = { .len = 7, .fx = { SLP_FX_GATE, SLP_FX_COMP, SLP_FX_DRIVE, SLP_FX_AMP, SLP_FX_CHORUS, SLP_FX_DELAY, SLP_FX_REVERB } };
    n = slp_set_chain_pack(&ch, p, sizeof(p));
    show("SET_CHAIN (seq 21): gate, comp, drive, amp, chorus, delay, reverb (then CAB)", SLP_SET_CHAIN, 21, 0, p, n);

    slp_set_master_t ms = { 63, 1 };
    n = slp_set_master_pack(&ms, p, sizeof(p));
    show("SET_MASTER (seq 22): volume 63, muted", SLP_SET_MASTER, 22, 0, p, n);

    slp_select_model_t sm = { SLP_MODEL_BUILTIN, 3, 0 };
    n = slp_select_model_pack(&sm, p, sizeof(p));
    show("SELECT_MODEL (seq 23): built-in amp 3 (Marshall JCM800 G5)", SLP_SELECT_MODEL, 23, 0, p, n);

    slp_snapshot_t sb = { 5, SLP_SNAP_HELLO };
    n = slp_snapshot_pack(&sb, p, sizeof(p));
    show("SNAPSHOT_BEGIN (seq 24): snapshot 5, reason 1 (peer HELLO)", SLP_SNAPSHOT_BEGIN, 24, 0, p, n);
    slp_snapshot_t se = { 5, 6 };
    n = slp_snapshot_pack(&se, p, sizeof(p));
    show("SNAPSHOT_END (seq 31): snapshot 5, 6 state frames sent in between", SLP_SNAPSHOT_END, 31, 0, p, n);

    slp_status_t st = { 553, 562, 0, SLP_MODEL_BUILTIN, 1, 0, SLP_STATUS_AUDIO_RUNNING, 5 };
    n = slp_status_pack(&st, p, sizeof(p));
    show("STATUS (seq 40): DSP 55.3 % avg / 56.2 % peak, 0 overruns, built-in amp 1, audio running, snapshot 5 applied",
         SLP_STATUS, 40, 0, p, n);

    slp_meters_t me = { -1830, -620, 0 };
    n = slp_meters_pack(&me, p, sizeof(p));
    show("METERS (seq 41): input -18.3 dBFS, output -6.2 dBFS, 0 clips", SLP_METERS, 41, 0, p, n);

    slp_model_begin_t mb = { 1, 1871, 7484, 0x9C3A5E21, "My TONE3000 Amp" };
    n = slp_model_begin_pack(&mb, p, sizeof(p));
    show("MODEL_BEGIN (seq 50, ack requested): transfer 1, 1871 weights, 7484 bytes, CRC32 0x9C3A5E21",
         SLP_MODEL_BEGIN, 50, SLP_FLAG_ACK_REQ, p, n);

    uint8_t data[8] = { 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xBF }; // 1.0f, -0.5f
    slp_model_chunk_t mc = { 1, 0, sizeof(data), data };
    n = slp_model_chunk_pack(&mc, p, sizeof(p));
    show("MODEL_CHUNK (seq 51, ack requested): transfer 1, offset 0, 8 bytes (real chunks carry up to 192)",
         SLP_MODEL_CHUNK, 51, SLP_FLAG_ACK_REQ, p, n);

    slp_ack_t ack = { SLP_MODEL_CHUNK, 51, SLP_OK };
    n = slp_ack_pack(&ack, p, sizeof(p));
    show("ACK from the Seed (seq 60): MODEL_CHUNK seq 51 OK", SLP_ACK, 60, 0, p, n);

    slp_model_end_t res = { 1, SLP_OK, 0x9C3A5E21 };
    n = slp_model_end_pack(&res, p, sizeof(p));
    show("MODEL_RESULT (seq 61): transfer 1 OK, Seed computed CRC32 0x9C3A5E21", SLP_MODEL_RESULT, 61, 0, p, n);
    return 0;
}
