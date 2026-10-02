// Seed link: the UART transport behind seed_link.h. One FreeRTOS task owns the UART and all
// protocol state; other tasks only copy the stats (under a spinlock) or wake the task through
// its UART event queue. Protocol: docs/SEED_LINK_PROTOCOL.md.
#include "seed_link.h"

#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "amp_models.h"
#include "nam_a2.h"
#include "nam_store.h"
#include "param_map_gen.h"
#include "rig_state.h"
#include "seed_link_proto.h"
#include "slp_coalesce.h"

static const char *TAG = "seed_link";

#define HELLO_PERIOD_US       500000
#define HEARTBEAT_PERIOD_US   1000000
#define LINK_TIMEOUT_US       3000000
#define SNAPSHOT_PERIOD_US    5000000
#define METERS_STALE_US       1000000
#define SNAPSHOT_CONFIRM_US   700000   // STATUS comes at 2 Hz; allow one period plus slack
#define RESYNC_MIN_GAP_US     1000000
#define COALESCE_MS           10
// The UART driver puts its rings in internal RAM (CONFIG_UART_ISR_IN_IRAM), which is scarce:
// 2 KB of RX is 20 ms of line time at 1 Mbaud, and the task drains it every few ms.
#define RX_RING               2048
#define TX_RING               0      // writes block until queued in the FIFO; a snapshot takes ~4 ms
#define EVENT_QUEUE_LEN       12
#define UP_WINDOW             4        // MODEL_CHUNKs in flight
#define UP_CHUNK_TIMEOUT_US   150000
#define UP_CHUNK_TRIES        5
#define UP_BEGIN_TIMEOUT_US   500000
#define UP_RESULT_TIMEOUT_US  3000000  // the Seed checks the CRC and stores 7.5 KB
#define UP_RETRY_AFTER_US     5000000  // after a failed upload, before trying again
enum { UP_IDLE, UP_BEGIN, UP_CHUNKS, UP_COMMIT };
#define TASK_STACK            6144
#define TASK_PRIORITY         5      // below the LVGL task (6), which runs on the other core
#define TASK_CORE             0
#define EVT_RIG_CHANGED       (UART_EVENT_MAX + 1)  // posted into the UART event queue by rig_state
#define EVT_PING              (UART_EVENT_MAX + 2)

#if CONFIG_SEED_LINK_ENABLE
#define LINK_UART ((uart_port_t)CONFIG_SEED_LINK_UART_NUM)
#endif

// --- shared with other tasks ---------------------------------------------------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static seed_link_stats_t s_stats;          // guarded by s_mux
static seed_link_builtin_t s_builtins[SEED_LINK_MAX_BUILTINS];
static int s_builtin_count;
static uint32_t s_builtins_version;
static QueueHandle_t s_events;
static char s_transport[64] = "disabled in menuconfig";

// --- link task state (only the link task touches these) -------------------------------------
typedef struct {
    slp_decoder_t dec;
    slp_coalescer_t coal;
    uint8_t seq;
    uint32_t boot_id;
    bool peer_heard;            // a peer HELLO has been received and the link hasn't timed out
    bool peer_known;            // we've had a HELLO since boot (peer_boot_id is valid)
    uint32_t peer_boot_id;
    int64_t last_rx_us, last_meters_us;
    int64_t next_hello_us, next_heartbeat_us, next_snapshot_us;
    uint8_t want_snapshot;      // SLP_SNAP_* reason, 0 = none
    bool pend_chain, pend_fx, pend_master;
    uint16_t snapshot_id;
    int64_t snapshot_sent_us;   // when the latest snapshot went out
    bool peer_confirms;         // the Seed has reported an applied snapshot id at least once
    int64_t last_resync_us;
    uint32_t last_flush_ms;
    bool flushed;
    uint32_t ping_token;
    uint32_t uart_frame_errors;
    // Counter bases for seed_link_reset_counters(): the Seed's counters are since its boot.
    uint32_t clip_base, overrun_base, err_base;
    // SD amp profile upload (MODEL_BEGIN / CHUNK / COMMIT)
    struct {
        int state;                  // UP_*
        uint32_t hash;              // CRC32 of the weights being uploaded
        uint8_t transfer_id;
        const uint8_t *data;        // packed weights in PSRAM (nam_store)
        uint32_t size, next_off, acked_bytes;
        uint8_t begin_seq, tries;
        int64_t deadline_us, retry_after_us;
        struct { bool used; uint8_t seq, tries; uint32_t off; int64_t sent_us; } win[UP_WINDOW];
        uint32_t seed_has[8];       // hashes the Seed has stored since its boot
        int seed_has_n;
    } up;
} link_t;
// In PSRAM (allocated in seed_link_init): internal RAM is reserved for what must be there.
static link_t *L;

static inline int64_t now_us(void) { return esp_timer_get_time(); }

// ---------------------------------------------------------------------------------------------
// Sending

#if CONFIG_SEED_LINK_ENABLE
static void send_frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t flags) {
    uint8_t wire[SLP_MAX_ENCODED];
    size_t n = slp_frame_encode(type, L->seq++, flags, payload, (uint16_t)len, wire, sizeof(wire));
    if (!n) {
        ESP_LOGE(TAG, "frame 0x%02x too large (%u bytes)", type, (unsigned)len);
        return;
    }
    // No TX ring: this returns once the frame is in the 128-byte hardware FIFO (about 2 ms for a
    // full frame at 1 Mbaud); the RX ring covers 20 ms meanwhile.
    uart_write_bytes(LINK_UART, wire, n);
    portENTER_CRITICAL(&s_mux);
    s_stats.packets_tx++;
    portEXIT_CRITICAL(&s_mux);
}

static void send_hello(void) {
    slp_hello_t h = { .proto_version = SLP_VERSION, .role = SLP_ROLE_UI, .boot_id = L->boot_id };
    strlcpy(h.fw_version, esp_app_get_description()->version, sizeof(h.fw_version));
    uint8_t p[SLP_MAX_PAYLOAD];
    send_frame(SLP_HELLO, p, slp_hello_pack(&h, p, sizeof(p)), 0);
}

// The amp's model as SELECT_MODEL: bypassed, an uploaded SD profile, or a built-in by the AmpId
// the Seed announced for that list position.
static slp_select_model_t amp_model(const rig_t *r) {
    slp_select_model_t m = { .kind = SLP_MODEL_BYPASS };
    if (!r->on[FX_AMP]) return m;
    if (r->amp_sd_hash) {
        m.kind = SLP_MODEL_UPLOADED;
        m.hash = r->amp_sd_hash;
        return m;
    }
    amp_model_t a;
    if (amp_models_get(r->model[FX_AMP], &a) && a.builtin) {
        m.kind = SLP_MODEL_BUILTIN;
        m.builtin_id = a.builtin_id;
    }
    return m; // an index outside the list (shouldn't happen) bypasses rather than guessing
}

static void send_chain(const rig_t *r) {
    slp_set_chain_t c = { .len = r->chain_len };
    for (int i = 0; i < r->chain_len; i++) c.fx[i] = k_fx_wire_id[r->chain[i]];
    uint8_t p[32];
    send_frame(SLP_SET_CHAIN, p, slp_set_chain_pack(&c, p, sizeof(p)), 0);
}

static void send_fx_state(const rig_t *r) {
    slp_set_fx_state_t s = { .count = UI_EFFECT_COUNT };
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        s.fx[fx] = (slp_fx_entry_t){ k_fx_wire_id[fx], r->on[fx], r->model[fx] };
    }
    uint8_t p[64];
    send_frame(SLP_SET_FX_STATE, p, slp_set_fx_state_pack(&s, p, sizeof(p)), 0);
    slp_select_model_t m = amp_model(r);
    send_frame(SLP_SELECT_MODEL, p, slp_select_model_pack(&m, p, sizeof(p)), 0);
}

static void send_master(const rig_t *r) {
    slp_set_master_t m = { (uint8_t)r->master_volume, r->muted ? 1 : 0 };
    uint8_t p[2];
    send_frame(SLP_SET_MASTER, p, slp_set_master_pack(&m, p, sizeof(p)), 0);
}

static void send_params(const slp_set_params_t *sp) {
    uint8_t p[SLP_MAX_PAYLOAD];
    send_frame(SLP_SET_PARAMS, p, slp_set_params_pack(sp, p, sizeof(p)), 0);
}

static unsigned slot_of(int fx, int knob) { return (unsigned)(fx * UI_MAX_KNOBS + knob); }

// The whole rig, bracketed so the Seed can confirm it got all of it.
static void send_snapshot(uint8_t reason) {
    rig_full_t r;
    rig_get_full(&r);
    const uint16_t id = ++L->snapshot_id;
    uint8_t p[8];
    slp_snapshot_t s = { id, reason };
    send_frame(SLP_SNAPSHOT_BEGIN, p, slp_snapshot_pack(&s, p, sizeof(p)), 0);

    send_chain(&r.r);
    send_fx_state(&r.r);     // SET_FX_STATE + SELECT_MODEL
    send_master(&r.r);
    uint8_t frames = 4;

    slp_set_params_t sp = { 0 };
    for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
        for (int k = 0; k < UI_MAX_KNOBS; k++) {
            if (!k_param_id[fx][k]) continue;
            slp_coal_set(&L->coal, slot_of(fx, k), k_param_id[fx][k], (int16_t)r.knobs[fx][k]);
            sp.params[sp.count++] = (slp_param_t){ k_param_id[fx][k], (int16_t)r.knobs[fx][k] };
            if (sp.count == SLP_MAX_PARAMS) {
                send_params(&sp);
                frames++;
                sp.count = 0;
            }
        }
    }
    if (sp.count) {
        send_params(&sp);
        frames++;
    }
    slp_coal_mark_all_sent(&L->coal);

    s.reason = frames;
    send_frame(SLP_SNAPSHOT_END, p, slp_snapshot_pack(&s, p, sizeof(p)), 0);

    L->pend_chain = L->pend_fx = L->pend_master = false;
    L->want_snapshot = 0;
    L->snapshot_sent_us = now_us();
    L->next_snapshot_us = L->snapshot_sent_us + SNAPSHOT_PERIOD_US;
    portENTER_CRITICAL(&s_mux);
    s_stats.snapshots_sent++;
    portEXIT_CRITICAL(&s_mux);
    if (reason != SLP_SNAP_PERIODIC) ESP_LOGI(TAG, "snapshot #%u sent (reason %u, %u frames)", id, reason, frames);
}

// ---------------------------------------------------------------------------------------------
// State changes from rig_state

static void take_rig_changes(void) {
    uint64_t knobs;
    const uint32_t dirty = rig_take_dirty(&knobs);
    if (!dirty) return;

    if (knobs) {
        rig_full_t r;
        rig_get_full(&r);
        for (int fx = 0; fx < UI_EFFECT_COUNT; fx++) {
            for (int k = 0; k < UI_MAX_KNOBS; k++) {
                const unsigned slot = slot_of(fx, k);
                if ((knobs >> slot) & 1u) slp_coal_set(&L->coal, slot, k_param_id[fx][k], (int16_t)r.knobs[fx][k]);
            }
        }
    }
    if (dirty & RIG_DIRTY_SNAPSHOT) L->want_snapshot = SLP_SNAP_PRESET;
    if (dirty & RIG_DIRTY_CHAIN) L->pend_chain = true;
    if (dirty & RIG_DIRTY_FX) L->pend_fx = true;
    if (dirty & RIG_DIRTY_MASTER) L->pend_master = true;
    // Mute never waits for coalescing.
    if ((dirty & RIG_DIRTY_MUTE_NOW) && L->peer_heard) {
        rig_full_t r;
        rig_get_full(&r);
        send_master(&r.r);
        L->pend_master = false;
    }
}

// Coalesced state: at most one burst of frames per COALESCE_MS, the first one at once.
static bool state_pending(void) { return L->pend_chain || L->pend_fx || L->pend_master || L->coal.pending; }

static uint32_t flush_wait_ms(uint32_t now_ms) {
    if (!state_pending()) return UINT32_MAX;
    const uint32_t since = now_ms - L->last_flush_ms;
    return (!L->flushed || since >= COALESCE_MS) ? 0 : COALESCE_MS - since;
}

static void flush_state(uint32_t now_ms) {
    if (flush_wait_ms(now_ms) != 0) return;
    // A consistent copy: the touch task may be rewriting the chain while this task reads it.
    rig_full_t full;
    rig_get_full(&full);
    const rig_t *r = &full.r;
    if (L->pend_chain) send_chain(r);
    if (L->pend_fx) send_fx_state(r);
    if (L->pend_master) send_master(r);
    L->pend_chain = L->pend_fx = L->pend_master = false;
    slp_set_params_t sp;
    while (slp_coal_take(&L->coal, now_ms, &sp)) send_params(&sp);
    L->last_flush_ms = now_ms;
    L->flushed = true;
}

static void on_rig_changed(void) {
    uart_event_t ev = { .type = (uart_event_type_t)EVT_RIG_CHANGED };
    xQueueSend(s_events, &ev, 0); // queue full means the task is already awake
}

// ---------------------------------------------------------------------------------------------
// Receiving

static void builtin_name(uint8_t id, char *out, size_t len) {
    snprintf(out, len, "built-in %u", id);
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_builtin_count; i++) {
        if (s_builtins[i].id == id) snprintf(out, len, "%s", s_builtins[i].name);
    }
    portEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------------------------
// Uploading SD amp profiles

static bool seed_has(uint32_t hash) {
    for (int i = 0; i < L->up.seed_has_n; i++) if (L->up.seed_has[i] == hash) return true;
    return false;
}

static void publish_upload(int state, uint8_t pct, const char *note) {
    portENTER_CRITICAL(&s_mux);
    s_stats.upload_state = (uint8_t)state;
    s_stats.upload_pct = pct;
    s_stats.upload_hash = L->up.hash;
    if (note) strlcpy(s_stats.upload_note, note, sizeof(s_stats.upload_note));
    portEXIT_CRITICAL(&s_mux);
}

static void upload_fail(const char *why) {
    ESP_LOGW(TAG, "upload of %08lx failed: %s", (unsigned long)L->up.hash, why);
    uint8_t b[8];
    slp_model_end_t ab = { L->up.transfer_id, SLP_ERR_TIMEOUT, 0 };
    send_frame(SLP_MODEL_ABORT, b, slp_model_end_pack(&ab, b, sizeof(b)), 0);
    L->up.state = UP_IDLE;
    L->up.retry_after_us = now_us() + UP_RETRY_AFTER_US;
    publish_upload(SEED_UPLOAD_FAILED, 0, why);
}

static void send_chunk(int slot, uint32_t off) {
    const uint32_t n = L->up.size - off < SLP_CHUNK_MAX ? L->up.size - off : SLP_CHUNK_MAX;
    slp_model_chunk_t c = { L->up.transfer_id, off, (uint8_t)n, L->up.data + off };
    uint8_t b[SLP_MAX_PAYLOAD];
    L->up.win[slot].seq = L->seq; // the seq send_frame is about to use
    L->up.win[slot].off = off;
    L->up.win[slot].sent_us = now_us();
    L->up.win[slot].used = true;
    send_frame(SLP_MODEL_CHUNK, b, slp_model_chunk_pack(&c, b, sizeof(b)), SLP_FLAG_ACK_REQ);
}

// Starts an upload if the selected amp is an SD profile the Seed doesn't hold yet.
static void upload_start_if_needed(int64_t now) {
    rig_full_t r;
    rig_get_full(&r);
    const uint32_t hash = r.r.amp_sd_hash;
    if (!hash || seed_has(hash) || L->up.state != UP_IDLE) return;
    if (hash == L->up.hash && now < L->up.retry_after_us) return;
    char name[64];
    const float *w = nam_store_weights(hash, name, sizeof(name));
    if (!w) return; // the profile isn't on the card (yet); the UI says so

    L->up.hash = hash;
    L->up.data = (const uint8_t *)w;
    L->up.size = NAM_A2_WEIGHT_COUNT * 4;
    L->up.next_off = L->up.acked_bytes = 0;
    L->up.tries = 0; // MODEL_BEGIN attempts; the retry path in upload_tick() carries its count over
    L->up.transfer_id++;
    memset(L->up.win, 0, sizeof(L->up.win));
    slp_model_begin_t b = { L->up.transfer_id, NAM_A2_WEIGHT_COUNT, L->up.size, hash, "" };
    strlcpy(b.name, name, sizeof(b.name));
    uint8_t p[64];
    L->up.begin_seq = L->seq;
    send_frame(SLP_MODEL_BEGIN, p, slp_model_begin_pack(&b, p, sizeof(p)), SLP_FLAG_ACK_REQ);
    L->up.state = UP_BEGIN;
    L->up.deadline_us = now + UP_BEGIN_TIMEOUT_US;
    ESP_LOGI(TAG, "uploading \"%s\" (%08lx) to the Seed", name, (unsigned long)hash);
    publish_upload(SEED_UPLOAD_RUNNING, 0, name);
}

static void upload_tick(int64_t now) {
    switch (L->up.state) {
    case UP_IDLE:
        upload_start_if_needed(now);
        return;
    case UP_BEGIN:
        if (now > L->up.deadline_us) {
            if (++L->up.tries >= UP_CHUNK_TRIES) {
                upload_fail("the Seed didn't answer MODEL_BEGIN");
                return;
            }
            const uint8_t tries = L->up.tries;
            L->up.state = UP_IDLE; // send BEGIN again
            L->up.retry_after_us = 0;
            upload_start_if_needed(now);
            L->up.tries = tries;
        }
        return;
    case UP_CHUNKS:
        for (int i = 0; i < UP_WINDOW; i++) {
            if (L->up.win[i].used && now - L->up.win[i].sent_us > UP_CHUNK_TIMEOUT_US) {
                if (L->up.win[i].tries >= UP_CHUNK_TRIES) {
                    upload_fail("a chunk was never acknowledged");
                    return;
                }
                const uint8_t tries = L->up.win[i].tries;
                send_chunk(i, L->up.win[i].off); // chunks are idempotent by offset
                L->up.win[i].tries = tries + 1;
            }
        }
        for (int i = 0; i < UP_WINDOW && L->up.next_off < L->up.size; i++) {
            if (!L->up.win[i].used) {
                send_chunk(i, L->up.next_off);
                L->up.win[i].tries = 1;
                L->up.next_off += SLP_CHUNK_MAX;
            }
        }
        if (L->up.next_off >= L->up.size) {
            bool pending = false;
            for (int i = 0; i < UP_WINDOW; i++) pending |= L->up.win[i].used;
            if (!pending) {
                uint8_t b[8];
                slp_model_end_t c = { L->up.transfer_id, 0, 0 };
                send_frame(SLP_MODEL_COMMIT, b, slp_model_end_pack(&c, b, sizeof(b)), 0);
                L->up.state = UP_COMMIT;
                L->up.deadline_us = now + UP_RESULT_TIMEOUT_US;
            }
        }
        return;
    case UP_COMMIT:
        if (now > L->up.deadline_us) upload_fail("no MODEL_RESULT from the Seed");
        return;
    }
}

static void upload_on_ack(const slp_ack_t *a) {
    if (a->acked_type == SLP_MODEL_BEGIN && L->up.state == UP_BEGIN && a->acked_seq == L->up.begin_seq) {
        if (a->status != SLP_OK) {
            upload_fail("the Seed refused the model");
            return;
        }
        L->up.state = UP_CHUNKS;
        return;
    }
    if (a->acked_type != SLP_MODEL_CHUNK || L->up.state != UP_CHUNKS) return;
    for (int i = 0; i < UP_WINDOW; i++) {
        if (L->up.win[i].used && L->up.win[i].seq == a->acked_seq) {
            if (a->status != SLP_OK) {
                upload_fail("the Seed rejected a chunk");
                return;
            }
            L->up.win[i].used = false;
            L->up.acked_bytes += SLP_CHUNK_MAX;
            const uint32_t done = L->up.acked_bytes < L->up.size ? L->up.acked_bytes : L->up.size;
            publish_upload(SEED_UPLOAD_RUNNING, (uint8_t)(done * 100 / L->up.size), NULL);
            return;
        }
    }
}

static void upload_on_result(const slp_model_end_t *r) {
    if (L->up.state != UP_COMMIT || r->transfer_id != L->up.transfer_id) return;
    if (r->status != SLP_OK || r->crc32 != L->up.hash) {
        upload_fail(r->status == SLP_ERR_CRC ? "CRC mismatch on the Seed" : "the Seed couldn't store the model");
        return;
    }
    if (L->up.seed_has_n < (int)(sizeof(L->up.seed_has) / sizeof(L->up.seed_has[0]))) {
        L->up.seed_has[L->up.seed_has_n++] = L->up.hash;
    }
    L->up.state = UP_IDLE;
    L->pend_fx = true; // SELECT_MODEL again, now that the Seed holds it
    ESP_LOGI(TAG, "upload of %08lx verified by the Seed (CRC match)", (unsigned long)L->up.hash);
    publish_upload(SEED_UPLOAD_DONE, 100, NULL);
}

static void on_hello(const slp_frame_t *f) {
    slp_hello_t h;
    if (!slp_hello_unpack(f->payload, f->len, &h)) return;
    if (h.role == SLP_ROLE_UI) {
        ESP_LOGW(TAG, "received our own kind of HELLO (UART looped back?)");
        return;
    }
    const bool is_new = !L->peer_known || h.boot_id != L->peer_boot_id;
    const bool was_heard = L->peer_heard;
    L->peer_known = true;
    L->peer_boot_id = h.boot_id;
    L->peer_heard = true;

    // Publish the peer's identity and its built-in models.
    seed_link_builtin_t list[SEED_LINK_MAX_BUILTINS];
    const int n = h.builtin_count < SEED_LINK_MAX_BUILTINS ? h.builtin_count : SEED_LINK_MAX_BUILTINS;
    for (int i = 0; i < n; i++) {
        list[i].id = h.builtins[i].id;
        snprintf(list[i].name, sizeof(list[i].name), "%s", h.builtins[i].name);
    }
    portENTER_CRITICAL(&s_mux);
    if (n != s_builtin_count || memcmp(list, s_builtins, sizeof(list[0]) * (size_t)n) != 0) {
        memcpy(s_builtins, list, sizeof(list[0]) * (size_t)n);
        s_builtin_count = n;
        s_builtins_version++;
    }
    s_stats.peer_mock = h.role == SLP_ROLE_MOCK_SEED;
    s_stats.peer_boot_id = h.boot_id;
    snprintf(s_stats.dsp_fw_version, sizeof(s_stats.dsp_fw_version), "%s", h.fw_version);
    s_stats.sample_rate_hz = h.sample_rate_hz;
    s_stats.block_size = h.block_size;
    portEXIT_CRITICAL(&s_mux);

    if (is_new) {
        ESP_LOGI(TAG, "%s HELLO: boot %08lx, fw \"%s\", %lu Hz / %u, %d built-in models",
                 h.role == SLP_ROLE_MOCK_SEED ? "MOCK Seed" : "Seed", (unsigned long)h.boot_id, h.fw_version,
                 (unsigned long)h.sample_rate_hz, h.block_size, n);
        send_hello();                   // answer each new peer boot once
        slp_coal_invalidate(&L->coal);   // the peer has none of our values
        L->up.seed_has_n = 0;            // nor any uploaded model
        if (L->up.state != UP_IDLE) {
            L->up.state = UP_IDLE;
            publish_upload(SEED_UPLOAD_NONE, 0, NULL);
        }
    }
    // Full state for a Seed we aren't in sync with: a new boot, or after the link was down. (A
    // repeat HELLO from the handshake itself needs nothing.)
    if (is_new || !was_heard) L->want_snapshot = SLP_SNAP_HELLO;
}

static void on_frame(const slp_frame_t *f) {
    const int64_t now = now_us();
    L->last_rx_us = now;
    portENTER_CRITICAL(&s_mux);
    s_stats.packets_rx++;
    portEXIT_CRITICAL(&s_mux);

    if (f->type == SLP_HELLO) {
        on_hello(f);
        return;
    }
    if (!L->peer_heard && L->peer_known) {
        // Frames again after a timeout, from the same peer boot: the cable or the line glitched.
        ESP_LOGI(TAG, "link back");
        L->peer_heard = true;
        L->want_snapshot = SLP_SNAP_RESYNC;
    }

    switch (f->type) {
    case SLP_PING:
        send_frame(SLP_PONG, f->payload, f->len, 0);
        break;
    case SLP_PONG: {
        slp_ping_t p;
        if (slp_ping_unpack(f->payload, f->len, &p) && p.token == L->ping_token) {
            const uint32_t rtt = (uint32_t)now - p.t_send_us;
            portENTER_CRITICAL(&s_mux);
            s_stats.rtt_ms = rtt / 1000.0f;
            portEXIT_CRITICAL(&s_mux);
        }
        break;
    }
    case SLP_STATUS: {
        slp_status_t st;
        if (!slp_status_unpack(f->payload, f->len, &st)) break;
        char model[40];
        if (st.model_kind == SLP_MODEL_BUILTIN) builtin_name(st.builtin_id, model, sizeof(model));
        else if (st.model_kind == SLP_MODEL_UPLOADED) snprintf(model, sizeof(model), "uploaded %08lx", (unsigned long)st.model_hash);
        else snprintf(model, sizeof(model), "bypass");
        if (st.flags & SLP_STATUS_MODEL_FAILED) strlcat(model, " (FAILED)", sizeof(model));
        portENTER_CRITICAL(&s_mux);
        s_stats.dsp_cpu_pct = st.cpu_avg_x10 / 10.0f;
        s_stats.dsp_cpu_peak_pct = st.cpu_peak_x10 / 10.0f;
        s_stats.dsp_overruns = st.overruns - L->overrun_base;
        s_stats.status_flags = st.flags;
        s_stats.active_kind = st.model_kind;
        s_stats.active_builtin = st.builtin_id;
        s_stats.active_hash = st.model_hash;
        s_stats.snapshot_applied = st.applied_snapshot_id;
        memcpy(s_stats.nam_model, model, sizeof(model));
        portEXIT_CRITICAL(&s_mux);

        // Fast healing: if the Seed hasn't confirmed the latest snapshot well after it went out,
        // a frame of it was lost; send it again now rather than at the next 5 s period. Only
        // for a Seed that reports applied ids at all, and at most once per second.
        if (st.applied_snapshot_id) L->peer_confirms = true;
        if (L->peer_confirms && !L->want_snapshot && st.applied_snapshot_id != L->snapshot_id && L->snapshot_id &&
            now - L->snapshot_sent_us > SNAPSHOT_CONFIRM_US && now - L->last_resync_us > RESYNC_MIN_GAP_US) {
            L->last_resync_us = now;
            L->want_snapshot = SLP_SNAP_RESYNC;
        }
        break;
    }
    case SLP_METERS: {
        slp_meters_t m;
        if (!slp_meters_unpack(f->payload, f->len, &m)) break;
        L->last_meters_us = now;
        portENTER_CRITICAL(&s_mux);
        s_stats.input_peak_dbfs = m.in_peak_cdb / 100.0f;
        s_stats.output_peak_dbfs = m.out_peak_cdb / 100.0f;
        s_stats.clip_count = m.clip_count - L->clip_base;
        s_stats.meters_valid = true;
        portEXIT_CRITICAL(&s_mux);
        break;
    }
    case SLP_ACK: {
        slp_ack_t a;
        if (slp_ack_unpack(f->payload, f->len, &a)) upload_on_ack(&a);
        break;
    }
    case SLP_MODEL_RESULT: {
        slp_model_end_t r;
        if (slp_model_end_unpack(f->payload, f->len, &r)) upload_on_result(&r);
        break;
    }
    case SLP_LOG:
        ESP_LOGI(TAG, "Seed: %.*s", f->len, (const char *)f->payload);
        break;
    default:
        break; // HEARTBEAT needs nothing beyond last_rx_us; unknown types are ignored
    }
}

static void read_uart(void) {
    uint8_t buf[256];
    size_t avail = 0;
    while (uart_get_buffered_data_len(LINK_UART, &avail) == ESP_OK && avail) {
        int n = uart_read_bytes(LINK_UART, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
        if (n <= 0) break;
        slp_frame_t f;
        for (int i = 0; i < n; i++) {
            if (slp_decoder_push(&L->dec, buf[i], &f)) on_frame(&f);
        }
    }
}

static void on_uart_event(const uart_event_t *ev) {
    switch ((int)ev->type) { // EVT_* extend the driver's enum
    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
        // Bytes were lost before the decoder saw them; the framing resynchronises by itself.
        portENTER_CRITICAL(&s_mux);
        s_stats.uart_overruns++;
        portEXIT_CRITICAL(&s_mux);
        read_uart();
        break;
    case UART_FRAME_ERR:
    case UART_PARITY_ERR:
        L->uart_frame_errors++;
        break;
    case EVT_PING:
        if (L->peer_heard) {
            slp_ping_t p = { ++L->ping_token, (uint32_t)now_us() };
            uint8_t b[8];
            send_frame(SLP_PING, b, slp_ping_pack(&p, b, sizeof(b)), 0);
        }
        break;
    default:
        break; // UART_DATA and EVT_RIG_CHANGED: handled by the loop after every wake-up
    }
}

// ---------------------------------------------------------------------------------------------
// Task

static uint32_t ms_until(int64_t t, int64_t now) {
    if (t <= now) return 0;
    const int64_t ms = (t - now + 999) / 1000;
    return ms > 1000 ? 1000 : (uint32_t)ms;
}

static void link_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t now = now_us();
        uint32_t wait = ms_until(L->next_heartbeat_us, now);
        if (!L->peer_heard) { uint32_t w = ms_until(L->next_hello_us, now); if (w < wait) wait = w; }
        if (L->peer_heard) {
            uint32_t w = ms_until(L->next_snapshot_us, now); if (w < wait) wait = w;
            w = flush_wait_ms((uint32_t)(now / 1000)); if (w < wait) wait = w;
            if (L->up.state != UP_IDLE && wait > 20) wait = 20;
        }

        uart_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(wait))) {
            do on_uart_event(&ev); while (xQueueReceive(s_events, &ev, 0));
        }
        read_uart();
        take_rig_changes();

        now = now_us();
        if (L->peer_heard && now - L->last_rx_us > LINK_TIMEOUT_US) {
            ESP_LOGW(TAG, "link lost (no frame for %d s)", LINK_TIMEOUT_US / 1000000);
            L->peer_heard = false;
            L->next_hello_us = now;
        }
        if (!L->peer_heard && now >= L->next_hello_us) {
            send_hello();
            L->next_hello_us = now + HELLO_PERIOD_US;
        }
        if (now >= L->next_heartbeat_us) {
            slp_heartbeat_t hb = { (uint32_t)(now / 1000), L->dec.frames_ok, slp_decoder_errors(&L->dec) };
            uint8_t b[12];
            send_frame(SLP_HEARTBEAT, b, slp_heartbeat_pack(&hb, b, sizeof(b)), 0);
            L->next_heartbeat_us = now + HEARTBEAT_PERIOD_US;
        }
        if (L->peer_heard) {
            if (L->want_snapshot) send_snapshot(L->want_snapshot);
            else if (now >= L->next_snapshot_us) send_snapshot(SLP_SNAP_PERIODIC);
            flush_state((uint32_t)(now / 1000));
            upload_tick(now);
        } else {
            // Nobody to send to: the snapshot on (re)connect carries everything.
            L->pend_chain = L->pend_fx = L->pend_master = false;
        }

        // Publish link health.
        const bool meters_fresh = L->peer_heard && now - L->last_meters_us < METERS_STALE_US;
        portENTER_CRITICAL(&s_mux);
        s_stats.connected = L->peer_heard;
        s_stats.meters_valid = meters_fresh;
        s_stats.link_errors = slp_decoder_errors(&L->dec) + L->uart_frame_errors - L->err_base;
        portEXIT_CRITICAL(&s_mux);
    }
}
#endif // CONFIG_SEED_LINK_ENABLE

// ---------------------------------------------------------------------------------------------
// Public API

void seed_link_init(void) {
#if CONFIG_SEED_LINK_ENABLE
    L = heap_caps_calloc(1, sizeof(*L), MALLOC_CAP_SPIRAM);
    if (!L) {
        ESP_LOGE(TAG, "no memory for the link state");
        return;
    }
    slp_decoder_init(&L->dec);
    slp_coal_init(&L->coal, COALESCE_MS);
    L->boot_id = esp_random();

    const uart_config_t cfg = {
        .baud_rate = CONFIG_SEED_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, RX_RING, TX_RING, EVENT_QUEUE_LEN, &s_events, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_SEED_LINK_TX_GPIO, CONFIG_SEED_LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    snprintf(s_transport, sizeof(s_transport), "UART%d %d baud, TX %d / RX %d", CONFIG_SEED_LINK_UART_NUM,
             CONFIG_SEED_LINK_BAUD, CONFIG_SEED_LINK_TX_GPIO, CONFIG_SEED_LINK_RX_GPIO);

    rig_set_notify(on_rig_changed);
    // Stack in PSRAM to spare internal RAM (about 60 KB minimum free is all the display leaves);
    // safe because this task never writes flash or runs with the cache disabled.
    if (xTaskCreatePinnedToCoreWithCaps(link_task, "seed_link", TASK_STACK, NULL, TASK_PRIORITY, NULL, TASK_CORE,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the link task");
        return;
    }
    ESP_LOGI(TAG, "started on %s, boot_id %08lx", s_transport, (unsigned long)L->boot_id);
#else
    ESP_LOGW(TAG, "disabled (CONFIG_SEED_LINK_ENABLE=n)");
#endif
}

void seed_link_get_stats(seed_link_stats_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}

bool seed_link_ping(void) {
    seed_link_stats_t s;
    seed_link_get_stats(&s);
    if (!s.connected || !s_events) return false;
    uart_event_t ev = { .type = (uart_event_type_t)EVT_PING };
    return xQueueSend(s_events, &ev, 0) == pdTRUE;
}

void seed_link_reset_counters(void) {
    if (!L) return;
    portENTER_CRITICAL(&s_mux);
    // The Seed's counters run since its boot; remember where they were instead.
    L->clip_base += s_stats.clip_count;
    L->overrun_base += s_stats.dsp_overruns;
    L->err_base += s_stats.link_errors;
    s_stats.packets_tx = 0;
    s_stats.packets_rx = 0;
    s_stats.link_errors = 0;
    s_stats.clip_count = 0;
    s_stats.dsp_overruns = 0;
    s_stats.uart_overruns = 0;
    s_stats.snapshots_sent = 0;
    portEXIT_CRITICAL(&s_mux);
}

const char *seed_link_transport(void) { return s_transport; }

int seed_link_get_builtins(seed_link_builtin_t *out, int max, uint32_t *version) {
    portENTER_CRITICAL(&s_mux);
    int n = s_builtin_count < max ? s_builtin_count : max;
    memcpy(out, s_builtins, sizeof(out[0]) * (size_t)n);
    if (version) *version = s_builtins_version;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
