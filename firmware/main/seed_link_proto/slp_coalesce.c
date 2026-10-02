#include "slp_coalesce.h"

#include <string.h>

static uint64_t bit(unsigned slot) { return (uint64_t)1 << slot; }

void slp_coal_init(slp_coalescer_t *c, uint32_t interval_ms) {
    memset(c, 0, sizeof(*c));
    c->interval_ms = interval_ms;
}

bool slp_coal_set(slp_coalescer_t *c, unsigned slot, uint16_t id, int16_t value) {
    if (slot >= SLP_COAL_SLOTS) return false;
    c->id[slot] = id;
    c->value[slot] = value;
    // Moving back to the value the peer already has cancels a pending update.
    if ((c->sent_valid & bit(slot)) && c->sent[slot] == value) c->pending &= ~bit(slot);
    else c->pending |= bit(slot);
    return true;
}

bool slp_coal_due(const slp_coalescer_t *c, uint32_t now_ms) {
    return c->pending && slp_coal_wait_ms(c, now_ms) == 0;
}

uint32_t slp_coal_wait_ms(const slp_coalescer_t *c, uint32_t now_ms) {
    if (!c->pending) return UINT32_MAX;
    if (!c->taken_once) return 0;
    const uint32_t elapsed = now_ms - c->last_take_ms; // unsigned: survives wrap
    return elapsed >= c->interval_ms ? 0 : c->interval_ms - elapsed;
}

unsigned slp_coal_take(slp_coalescer_t *c, uint32_t now_ms, slp_set_params_t *out) {
    unsigned n = 0;
    for (unsigned s = 0; s < SLP_COAL_SLOTS && n < SLP_MAX_PARAMS; s++) {
        if (!(c->pending & bit(s))) continue;
        out->params[n].id = c->id[s];
        out->params[n].value = c->value[s];
        n++;
        c->sent[s] = c->value[s];
        c->sent_valid |= bit(s);
        c->pending &= ~bit(s);
    }
    out->count = (uint8_t)n;
    if (n) {
        c->last_take_ms = now_ms;
        c->taken_once = true;
    }
    return n;
}

void slp_coal_invalidate(slp_coalescer_t *c) {
    c->pending |= c->sent_valid;
    c->sent_valid = 0;
}

void slp_coal_mark_all_sent(slp_coalescer_t *c) {
    for (unsigned s = 0; s < SLP_COAL_SLOTS; s++) {
        if ((c->pending | c->sent_valid) & bit(s)) {
            c->sent[s] = c->value[s];
            c->sent_valid |= bit(s);
        }
    }
    c->pending = 0;
}
