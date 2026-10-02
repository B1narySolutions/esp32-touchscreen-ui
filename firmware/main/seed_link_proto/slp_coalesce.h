/*
 * Parameter coalescing for the sending side (the ESP32). Pure C99, no allocation.
 *
 * A slider drag changes a value on every touch sample; the link must not send a frame for each.
 * Values are recorded per slot (a small caller-chosen index, e.g. effect * 5 + knob, each with
 * its wire parameter id). At most one SET_PARAMS frame goes out per interval, carrying only
 * slots whose latest value differs from what was last sent. The first change after a quiet
 * period goes out at once (leading edge), so a single tap has no added latency.
 */
#ifndef SLP_COALESCE_H
#define SLP_COALESCE_H

#include "seed_link_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_COAL_SLOTS 64

typedef struct {
    uint16_t id[SLP_COAL_SLOTS];
    int16_t value[SLP_COAL_SLOTS];     // latest value
    int16_t sent[SLP_COAL_SLOTS];      // value last handed out by slp_coal_take
    uint64_t pending;                  // value != sent, or never sent
    uint64_t sent_valid;
    uint32_t interval_ms;
    uint32_t last_take_ms;
    bool taken_once;
} slp_coalescer_t;

void slp_coal_init(slp_coalescer_t *c, uint32_t interval_ms);
// Records the latest value of a slot. Returns false for an out-of-range slot.
bool slp_coal_set(slp_coalescer_t *c, unsigned slot, uint16_t id, int16_t value);
// True if something is pending and the interval since the last take has passed.
bool slp_coal_due(const slp_coalescer_t *c, uint32_t now_ms);
// Milliseconds until slp_coal_due() becomes true (0 = now), or UINT32_MAX if nothing pends.
uint32_t slp_coal_wait_ms(const slp_coalescer_t *c, uint32_t now_ms);
// Moves up to SLP_MAX_PARAMS pending slots into out (lowest slot first), marks them sent and
// restarts the interval. Returns the number taken (0 if nothing pending).
unsigned slp_coal_take(slp_coalescer_t *c, uint32_t now_ms, slp_set_params_t *out);
// Forget what the peer has (it rebooted): every slot with a value becomes pending again.
void slp_coal_invalidate(slp_coalescer_t *c);
// A full snapshot just carried every value: nothing is pending, everything counts as sent.
void slp_coal_mark_all_sent(slp_coalescer_t *c);

#ifdef __cplusplus
}
#endif
#endif
