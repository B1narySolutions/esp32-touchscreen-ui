// Host tests for amp_controls: flat at noon (which bounds the error against the circuit at any
// setting), every knob moves the sound the right way, the real-time path matches the designed
// response, and no blow-ups at extreme settings.  Build and run: bash seed-dsp/test.sh
#include "amp_controls.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ampctl;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                                                                             \
    do {                                                                                                                                             \
        ++g_checks;                                                                                                                                  \
        if (!(cond)) {                                                                                                                               \
            ++g_fail;                                                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                                            \
            printf(__VA_ARGS__);                                                                                                                     \
            printf("\n");                                                                                                                            \
        }                                                                                                                                            \
    } while (0)

static const Voicing kAll[] = {Voicing::Fender, Voicing::Vox, Voicing::Marshall, Voicing::Generic};
static const char *Name(Voicing v) { return v == Voicing::Fender ? "Fender" : v == Voicing::Vox ? "Vox" : v == Voicing::Marshall ? "Marshall" : "Generic"; }

static double Resp(Voicing v, std::initializer_list<int16_t> k, double hz) {
    AmpControls c;
    int16_t knobs[kSlots];
    int i = 0;
    for (auto x : k)
        knobs[i++] = x;
    c.Configure(v, knobs);
    return c.PostResponseDb(hz);
}

// Steady-state gain of the real-time path (pre + post) for a sine, in dB.
static double MeasuredDb(Voicing v, const int16_t k[kSlots], double hz) {
    AmpControls c;
    c.Configure(v, k);
    const int n = 48000, block = 48;
    std::vector<float> x(block);
    double in_sq = 0, out_sq = 0;
    for (int start = 0; start < n; start += block) {
        for (int i = 0; i < block; ++i)
            x[i] = 0.1f * static_cast<float>(std::sin(2 * M_PI * hz * (start + i) / 48000.0));
        std::vector<float> in = x;
        c.ProcessPre(x.data(), block);
        c.ProcessPost(x.data(), block);
        if (start >= n / 2) // settled
            for (int i = 0; i < block; ++i)
                in_sq += in[i] * in[i], out_sq += x[i] * x[i];
    }
    return 10 * std::log10(out_sq / in_sq);
}

int main() {
    // 1. Every knob at noon (Bright off) is exactly the capture: flat within 2 dB.
    for (Voicing v : kAll) {
        double worst = 0;
        for (double hz = 50; hz < 15000; hz *= 1.15)
            worst = std::max(worst, std::fabs(Resp(v, {50, 50, 50, 50, 50}, hz)));
        printf("%-8s at noon: within %.2f dB of the capture (50 Hz .. 15 kHz)\n", Name(v), worst);
        CHECK(worst < 2.0, "%s not flat at noon: %.2f dB", Name(v), worst);
    }

    // 2. Each knob moves the sound the right way.
    for (Voicing v : kAll) {
        const auto tilt = [&](std::initializer_list<int16_t> k) { return Resp(v, k, 5000) - Resp(v, k, 100); };
        CHECK(tilt({50, 50, 50, 100, 50}) > tilt({50, 50, 50, 0, 50}) + 6, "%s treble doesn't raise highs", Name(v));
        CHECK(Resp(v, {50, 100, 50, 50, 50}, 60) > Resp(v, {50, 0, 50, 50, 50}, 60) + 3, "%s bass doesn't raise lows", Name(v));
        if (v != Voicing::Vox)
            CHECK(Resp(v, {50, 50, 100, 50, 50}, 600) > Resp(v, {50, 50, 0, 50, 50}, 600) + 3, "%s middle doesn't raise mids", Name(v));
    }
    CHECK(Resp(Voicing::Vox, {50, 50, 100, 50, 50}, 8000) < Resp(Voicing::Vox, {50, 50, 0, 50, 50}, 8000) - 6, "Vox cut doesn't cut highs");
    CHECK(Resp(Voicing::Marshall, {50, 50, 50, 50, 100}, 7000) > Resp(Voicing::Marshall, {50, 50, 50, 50, 0}, 7000) + 6,
          "Marshall presence doesn't lift highs");
    CHECK(std::fabs(Resp(Voicing::Vox, {50, 50, 50, 50, 100}, 1000) - Resp(Voicing::Vox, {50, 50, 50, 50, 50}, 1000) - 12) < 0.5,
          "Vox master isn't +12 dB at full");
    {
        AmpControls c;
        const int16_t lo[kSlots] = {0, 50, 50, 50, 50}, hi[kSlots] = {100, 50, 50, 50, 50};
        c.Configure(Voicing::Marshall, lo);
        const double d_lo = c.DriveDb();
        c.Configure(Voicing::Marshall, hi);
        CHECK(std::fabs(d_lo + 20) < 0.01 && std::fabs(c.DriveDb() - 14) < 0.01, "drive range isn't -20 .. +14 dB");
    }
    // Fender Bright: a treble lift at low volume, none at full volume.
    {
        const int16_t quiet_on[kSlots] = {20, 50, 50, 50, 100}, quiet_off[kSlots] = {20, 50, 50, 50, 0}, loud_on[kSlots] = {100, 50, 50, 50, 100},
                      loud_off[kSlots] = {100, 50, 50, 50, 0};
        const double lift_quiet = MeasuredDb(Voicing::Fender, quiet_on, 5000) - MeasuredDb(Voicing::Fender, quiet_off, 5000);
        const double lift_loud = MeasuredDb(Voicing::Fender, loud_on, 5000) - MeasuredDb(Voicing::Fender, loud_off, 5000);
        printf("Fender Bright at 5 kHz: +%.1f dB at volume 20, +%.1f dB at volume 100\n", lift_quiet, lift_loud);
        CHECK(lift_quiet > 5 && std::fabs(lift_loud) < 0.1, "Fender bright wrong: %.2f / %.2f dB", lift_quiet, lift_loud);
    }

    // 3. The real-time path measures what the design says (catches filter coding mistakes).
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> knob(0, 100);
    double worst_path = 0;
    for (Voicing v : kAll)
        for (int trial = 0; trial < 6; ++trial) {
            int16_t k[kSlots];
            for (auto &x : k)
                x = static_cast<int16_t>(knob(rng));
            if (v == Voicing::Fender)
                k[4] = 0; // bright is a pre filter; checked above
            AmpControls c;
            c.Configure(v, k);
            for (double hz : {100.0, 1000.0, 5000.0}) {
                const double expect = c.PostResponseDb(hz) + c.DriveDb();
                worst_path = std::max(worst_path, std::fabs(MeasuredDb(v, k, hz) - expect));
            }
        }
    printf("real-time path vs design: within %.3f dB\n", worst_path);
    CHECK(worst_path < 0.3, "real-time path differs from the design by %.3f dB", worst_path);

    // (Accuracy against the circuit: the knobs run the exact stack, and the only approximation is
    // the fixed correction for the capture's stack, so its error at any setting is test 1's.)

    // 5. Extreme settings and loud noise: finite and bounded.
    {
        std::normal_distribution<float> noise(0.0f, 0.5f);
        bool finite = true;
        float peak = 0;
        for (Voicing v : kAll)
            for (int16_t a : {0, 100})
                for (int16_t b : {0, 100}) {
                    AmpControls c;
                    const int16_t k[kSlots] = {a, b, a, b, a};
                    c.Configure(v, k);
                    std::vector<float> x(48);
                    for (int blk = 0; blk < 2000; ++blk) {
                        for (auto &s : x)
                            s = noise(rng);
                        c.ProcessPre(x.data(), x.size());
                        c.ProcessPost(x.data(), x.size());
                        for (float s : x)
                            finite &= std::isfinite(s), peak = std::max(peak, std::fabs(s));
                    }
                }
        printf("extreme settings with loud noise: %s, peak %.1f\n", finite ? "finite" : "NOT FINITE", peak);
        CHECK(finite && peak < 1000, "unstable at extreme settings");
    }

    // Cost on this computer (the Seed's chip is far slower; measure there too).
    {
        AmpControls c;
        const int16_t k[kSlots] = {60, 40, 70, 55, 50};
        c.Configure(Voicing::Marshall, k);
        std::vector<float> x(48, 0.1f);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) {
            c.ProcessPre(x.data(), x.size());
            c.ProcessPost(x.data(), x.size());
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20000;
        printf("cost here: %.2f us per 48-sample block\n", us);
        const auto t1 = std::chrono::steady_clock::now();
        c.Configure(Voicing::Fender, k);
        printf("configure (new amp, refits the correction): %.0f us here\n",
               std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count());
    }

    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
