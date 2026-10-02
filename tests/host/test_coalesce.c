// Coalescer: a drag produces a bounded number of frames, the final value always arrives, and
// returning to the already-sent value cancels the update.
#include <string.h>
#include "slp_coalesce.h"
#include "test.h"

void test_coalesce(void) {
    slp_coalescer_t c;
    slp_set_params_t out;

    // Leading edge: the first change goes out at once.
    slp_coal_init(&c, 10);
    CHECK(!slp_coal_due(&c, 0));
    CHECK_EQ(slp_coal_wait_ms(&c, 0), UINT32_MAX);
    slp_coal_set(&c, 20, 0x0500, 40);
    CHECK(slp_coal_due(&c, 1000));
    CHECK_EQ(slp_coal_take(&c, 1000, &out), 1);
    CHECK(out.params[0].id == 0x0500 && out.params[0].value == 40);

    // Within the interval nothing is due; wait_ms says how long.
    slp_coal_set(&c, 20, 0x0500, 41);
    CHECK(!slp_coal_due(&c, 1003));
    CHECK_EQ(slp_coal_wait_ms(&c, 1003), 7);
    CHECK(slp_coal_due(&c, 1010));

    // Back to the value the peer has: nothing pending.
    slp_coal_set(&c, 20, 0x0500, 40);
    CHECK(!slp_coal_due(&c, 2000));

    // A 2 s drag with a new value every millisecond (far faster than touch) on two knobs at
    // once: at most one frame per 10 ms, each with both knobs, and the last values arrive.
    slp_coal_init(&c, 10);
    unsigned frames = 0;
    int last_a = -1, last_b = -1;
    for (uint32_t t = 0; t < 2000; t++) {
        slp_coal_set(&c, 3, 0x0103, (int16_t)(t % 101));
        slp_coal_set(&c, 4, 0x0200, (int16_t)(100 - t % 101));
        if (slp_coal_due(&c, t)) {
            unsigned n = slp_coal_take(&c, t, &out);
            frames++;
            for (unsigned k = 0; k < n; k++) {
                if (out.params[k].id == 0x0103) last_a = out.params[k].value;
                if (out.params[k].id == 0x0200) last_b = out.params[k].value;
            }
        }
    }
    // Drag ends: flush the tail once the interval passes.
    if (slp_coal_due(&c, 2010)) {
        unsigned n = slp_coal_take(&c, 2010, &out);
        frames++;
        for (unsigned k = 0; k < n; k++) {
            if (out.params[k].id == 0x0103) last_a = out.params[k].value;
            if (out.params[k].id == 0x0200) last_b = out.params[k].value;
        }
    }
    CHECK(frames <= 2000 / 10 + 2);
    CHECK(frames >= 2000 / 10 - 2);
    CHECK_EQ(last_a, 1999 % 101);
    CHECK_EQ(last_b, 100 - 1999 % 101);
    CHECK(!c.pending);

    // More than one frame's worth pending: taken in two frames, lowest slots first.
    slp_coal_init(&c, 10);
    for (unsigned s = 0; s < SLP_COAL_SLOTS; s++) slp_coal_set(&c, s, (uint16_t)(0x100 + s), (int16_t)s);
    CHECK_EQ(slp_coal_take(&c, 0, &out), SLP_MAX_PARAMS);
    CHECK_EQ(out.params[0].id, 0x100);
    CHECK_EQ(slp_coal_take(&c, 10, &out), SLP_COAL_SLOTS - SLP_MAX_PARAMS);
    CHECK_EQ(out.params[0].id, 0x100 + SLP_MAX_PARAMS);

    // Peer rebooted: everything known becomes pending again.
    slp_coal_invalidate(&c);
    CHECK_EQ(slp_coal_take(&c, 20, &out), SLP_MAX_PARAMS);
    CHECK_EQ(slp_coal_take(&c, 30, &out), SLP_COAL_SLOTS - SLP_MAX_PARAMS);

    // A snapshot carried everything: nothing pending, and a repeat of the same value stays quiet.
    slp_coal_set(&c, 5, 0x105, 99);
    slp_coal_mark_all_sent(&c);
    CHECK(!c.pending);
    slp_coal_set(&c, 5, 0x105, 99);
    CHECK(!c.pending);

    // Bad slot and clock wrap.
    CHECK(!slp_coal_set(&c, SLP_COAL_SLOTS, 1, 1));
    slp_coal_init(&c, 10);
    slp_coal_set(&c, 0, 1, 1);
    slp_coal_take(&c, 0xFFFFFFFAu, &out);
    slp_coal_set(&c, 0, 1, 2);
    CHECK(!slp_coal_due(&c, 0xFFFFFFFFu));
    CHECK(slp_coal_due(&c, 4)); // 10 ms after 0xFFFFFFFA, across the wrap
}
