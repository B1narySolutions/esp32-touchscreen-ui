#include "seed_link.h"
#include <string.h>

// No transport yet: the Daisy Seed isn't wired in. Everything reports "not connected"
// until the bus driver is written; only this file needs to change when it is.
static seed_link_stats_t s_stats;

void seed_link_get_stats(seed_link_stats_t *out) {
    *out = s_stats;
}

bool seed_link_ping(void) {
    return s_stats.connected;
}

void seed_link_reset_counters(void) {
    s_stats.packets_tx = 0;
    s_stats.packets_rx = 0;
    s_stats.link_errors = 0;
    s_stats.clip_count = 0;
}
