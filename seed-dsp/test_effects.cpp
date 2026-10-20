// Host tests for effects (delay and reverb): echo times and repeat levels, measured decay times,
// level at full MIX, pre-delay, tone, exact dry signal at MIX 0, no clicks on TIME changes, trails,
// and no blow-ups at extreme settings.  Build and run: bash seed-dsp/test.sh
#include "effects.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <utility>
#include <vector>

using namespace fx;

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

constexpr int kBlock = 48;
static const char *kDelayNames[] = {"Digital", "Tape"};
static const char *kReverbNames[] = {"Hall", "Room", "Plate", "Spring"};

// Runs `fx` over `in` in 48-sample blocks; returns the output.
template <class Fx> static std::vector<float> Run(Fx &fx, std::vector<float> in) {
    for (size_t s = 0; s + kBlock <= in.size(); s += kBlock)
        fx.Process(in.data() + s, kBlock);
    return in;
}

// Noise shaped roughly like a guitar through a cabinet: two one-pole low-passes at 3 kHz.
static std::vector<float> GuitarNoise(size_t n, float rms) {
    std::mt19937 rng(7);
    std::normal_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(n);
    const float a = std::exp(-2 * float(M_PI) * 3000.0f / 48000.0f);
    float z1 = 0, z2 = 0;
    double sq = 0;
    for (auto &x : v) {
        z1 += (1 - a) * (d(rng) - z1);
        z2 += (1 - a) * (z1 - z2);
        x = z2;
        sq += double(x) * x;
    }
    const float scale = rms / float(std::sqrt(sq / double(n)));
    for (auto &x : v)
        x *= scale;
    return v;
}

static std::vector<float> Noise(size_t n, float rms, unsigned seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.0f, rms);
    std::vector<float> v(n);
    for (auto &x : v)
        x = d(rng);
    return v;
}

static double Rms(const std::vector<float> &v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i)
        s += double(v[i]) * v[i];
    return std::sqrt(s / double(to - from));
}

static bool Finite(const std::vector<float> &v, float limit) {
    for (float x : v)
        if (!std::isfinite(x) || std::fabs(x) > limit)
            return false;
    return true;
}

// RT60 of an impulse response in the 1 kHz octave band (as decay times are usually quoted; the
// damping makes the treble die sooner): Schroeder backward integration, slope from -5 to -25 dB.
static double Rt60(const std::vector<float> &raw) {
    // RBJ band-pass at 1 kHz, one octave wide.
    const double w = 2 * M_PI * 1000.0 / 48000.0, alpha = std::sin(w) * std::sinh(std::log(2.0) / 2 * 1.0 * w / std::sin(w));
    const double a0 = 1 + alpha, b0 = alpha / a0, b2 = -alpha / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - alpha) / a0;
    std::vector<double> h(raw.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        const double y = b0 * raw[i] + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = raw[i], y2 = y1, y1 = y, h[i] = y;
    }
    std::vector<double> e(h.size() + 1, 0.0);
    for (size_t i = h.size(); i-- > 0;)
        e[i] = e[i + 1] + h[i] * h[i];
    auto at = [&](double db) {
        for (size_t i = 0; i < h.size(); ++i)
            if (10 * std::log10(e[i] / e[0] + 1e-30) <= db)
                return double(i);
        return double(h.size());
    };
    return 3.0 * (at(-25) - at(-5)) / 48000.0;
}

// Energy in a band (simple DFT bins), for tone comparisons.
static double BandEnergy(const std::vector<float> &v, size_t from, size_t len, double lo, double hi) {
    double sum = 0;
    for (double f = lo; f <= hi; f *= 1.12) {
        double re = 0, im = 0;
        for (size_t i = 0; i < len; ++i) {
            const double w = 2 * M_PI * f * double(i) / 48000.0;
            re += v[from + i] * std::cos(w);
            im += v[from + i] * std::sin(w);
        }
        sum += re * re + im * im;
    }
    return sum;
}

static void TestDelay() {
    std::vector<float> mem(Delay::kMemFloats);

    // 1. TIME and FEEDBACK mappings.
    CHECK(std::fabs(Delay::TimeMs(0) - 40) < 0.5 && std::fabs(Delay::TimeMs(100) - 1000) < 5, "TIME 0 -> %.1f ms, 100 -> %.1f ms", Delay::TimeMs(0),
          Delay::TimeMs(100));

    for (int model = 0; model < 2; ++model) {
        // 2. Echoes land at TIME, and each repeat is FEEDBACK times the one before (Digital: the
        //    loop's low-pass has unity gain at DC, so the sum over an echo is its level).
        for (int16_t t : {0, 30, 60, 100}) {
            Delay d;
            d.Init(mem.data());
            const int16_t k[4] = {t, 50, 100, 100};
            d.Set(true, uint8_t(model), k);
            std::vector<float> in(48000 * 4, 0.0f);
            const size_t start = 48000; // after the crossfade to TIME
            in[start] = 1.0f;
            auto out = Run(d, in);
            const double delay = Delay::TimeMs(t) * 48.0;
            size_t peak = start + 1;
            for (size_t i = start + 1; i < start + size_t(delay * 1.5) && i < out.size(); ++i)
                if (std::fabs(out[i]) > std::fabs(out[peak]))
                    peak = i;
            const double err = double(peak - start) - delay;
            // Tape: wow moves the echo by up to 13 samples and its low-pass spreads it.
            CHECK(std::fabs(err) <= (model ? 40 : 2), "%s TIME %d: echo at %zu samples, expected %.0f", kDelayNames[model], t, peak - start, delay);
            if (model == 0 && t >= 30) {
                auto sum = [&](double c) {
                    double s = 0;
                    for (long i = long(c) - 400; i < long(c) + 400; ++i)
                        s += out[size_t(i)];
                    return s;
                };
                const double e1 = sum(start + delay), e2 = sum(start + 2 * delay);
                CHECK(std::fabs(e1 - 1.0) < 0.02 && std::fabs(e2 / e1 - Delay::Feedback(50)) < 0.02,
                      "Digital TIME %d: first echo %.3f (expected 1), repeat ratio %.3f (expected %.3f)", t, e1, e2 / e1, Delay::Feedback(50));
            }
        }

        // 3. MIX 0, and an effect never turned on, leave the signal exactly as it was.
        {
            auto in = Noise(48000, 0.2f);
            Delay a, b;
            std::vector<float> mem2(Delay::kMemFloats);
            a.Init(mem.data());
            b.Init(mem2.data());
            const int16_t k[4] = {50, 60, 70, 0};
            a.Set(true, uint8_t(model), k);
            auto oa = Run(a, in), ob = Run(b, in);
            CHECK(oa == in && ob == in, "%s: MIX 0 / never on is not exactly the dry signal", kDelayNames[model]);
        }

        // 4. Extreme settings with loud noise stay finite and bounded.
        {
            Delay d;
            d.Init(mem.data());
            const int16_t k[4] = {0, 100, 100, 100};
            d.Set(true, uint8_t(model), k);
            auto out = Run(d, Noise(48000 * 20, 0.7f));
            CHECK(Finite(out, 12.0f), "%s: blew up at FEEDBACK 100", kDelayNames[model]);
        }

        // 5. Turning TIME while a tone plays never steps the output (crossfade).
        {
            Delay d;
            d.Init(mem.data());
            int16_t k[4] = {30, 40, 100, 60};
            d.Set(true, uint8_t(model), k);
            std::vector<float> in(48000 * 3);
            for (size_t i = 0; i < in.size(); ++i)
                in[i] = 0.5f * float(std::sin(2 * M_PI * 220.0 * double(i) / 48000.0));
            std::vector<float> out = in;
            double before = 0, after = 0;
            for (size_t s = 0; s + kBlock <= out.size(); s += kBlock) {
                if (s == 48000 * 2) {
                    k[0] = 70;
                    d.Set(true, uint8_t(model), k);
                }
                d.Process(out.data() + s, kBlock);
                for (size_t i = s ? s : 1; i < s + kBlock; ++i) {
                    const double step = std::fabs(out[i] - out[i - 1]);
                    (s < 48000 * 2 ? before : after) = std::max(s < 48000 * 2 ? before : after, s >= 48000 ? step : 0.0);
                }
            }
            CHECK(after < 1.6 * before, "%s: TIME change made a step of %.4f (steady %.4f)", kDelayNames[model], after, before);
        }

        // 6. Trails: echoes of what was played keep going after OFF; nothing played after OFF is
        //    echoed; then the effect goes idle.
        {
            Delay d;
            d.Init(mem.data());
            int16_t k[4] = {40, 30, 100, 100};
            d.Set(true, uint8_t(model), k);
            std::vector<float> in(48000 * 6, 0.0f);
            in[48000] = 1.0f;     // played while on
            in[48000 + 960] = 1.0f; // played 20 ms after OFF
            std::vector<float> out = in;
            for (size_t s = 0; s + kBlock <= out.size(); s += kBlock) {
                if (s == 48000 + 480)
                    d.Set(false, uint8_t(model), k);
                d.Process(out.data() + s, kBlock);
            }
            const size_t delay = size_t(Delay::TimeMs(40) * 48.0);
            double trail = 0, ghost = 0;
            for (size_t i = 48000 + delay - 60; i < 48000 + delay + 60; ++i)
                trail = std::max(trail, double(std::fabs(out[i])));
            for (size_t i = 48000 + 960 + delay - 60; i < 48000 + 960 + delay + 60; ++i)
                ghost = std::max(ghost, double(std::fabs(out[i])));
            CHECK(trail > 0.1 && ghost < 0.02 && !d.Active(), "%s trails: echo after OFF %.3f, echo of audio played after OFF %.4f, idle %d",
                  kDelayNames[model], trail, ghost, !d.Active());
        }
    }
}

static void TestReverb() {
    std::vector<float> mem(Reverb::kMemFloats);
    CHECK(std::fabs(Reverb::DecaySeconds(Reverb::kHall, 0) - 1.0) < 0.01 && std::fabs(Reverb::DecaySeconds(Reverb::kHall, 100) - 8.0) < 0.05 &&
              std::fabs(Reverb::PredelayMs(50) - 50) < 0.01,
          "DECAY / PRE-DELAY mappings");

    for (int model = 0; model < 4; ++model) {
        const char *name = kReverbNames[model];
        // 1. The measured decay time matches DECAY.
        for (int16_t dk : {20, 60}) {
            Reverb r;
            r.Init(mem.data());
            const int16_t k[4] = {dk, 0, 100, 100};
            r.Set(true, uint8_t(model), k);
            std::vector<float> in(size_t(48000 * (Reverb::DecaySeconds(uint8_t(model), dk) * 1.6 + 0.5)), 0.0f);
            in[0] = 1.0f;
            auto out = Run(r, in);
            out[0] = 0; // the dry impulse
            const double want = Reverb::DecaySeconds(uint8_t(model), dk), got = Rt60(out);
            const double tol = 0.1;
            CHECK(std::fabs(got / want - 1) < tol, "%s DECAY %d: RT60 %.2f s, expected %.2f s", name, dk, got, want);
            printf("  %-6s DECAY %3d: RT60 %.2f s (set %.2f s)\n", name, dk, got, want);
        }

        // 2. Full MIX is about as loud as the dry signal, whatever DECAY and TONE (guitar-like
        //    noise in).
        for (auto [dk, tk] : {std::pair<int16_t, int16_t>{10, 100}, {90, 100}, {50, 0}, {50, 50}}) {
            Reverb r;
            r.Init(mem.data());
            const int16_t k[4] = {dk, 0, tk, 100};
            r.Set(true, uint8_t(model), k);
            auto in = GuitarNoise(48000 * 6, 0.1f);
            auto out = Run(r, in);
            for (size_t i = 0; i < out.size(); ++i)
                out[i] -= in[i];
            const double db = 20 * std::log10(Rms(out, 48000 * 4, out.size()) / 0.1);
            CHECK(db > -6 && db < 4, "%s DECAY %d TONE %d: reverb at MIX 100 is %.1f dB re the dry signal", name, dk, tk, db);
            printf("  %-6s DECAY %3d TONE %3d: wet level %+.1f dB\n", name, dk, tk, db);
        }

        // 3. PRE-DELAY: nothing comes back before it.
        {
            Reverb r;
            r.Init(mem.data());
            const int16_t k[4] = {50, 50, 100, 100};
            r.Set(true, uint8_t(model), k);
            std::vector<float> in(48000, 0.0f);
            in[0] = 1.0f;
            auto out = Run(r, in);
            out[0] = 0;
            size_t first = 0;
            while (first < out.size() && std::fabs(out[first]) < 1e-5f)
                ++first;
            CHECK(first >= 2400 && first < 2400 + 2500, "%s PRE-DELAY 50 ms: first reflection at %.1f ms", name, first / 48.0);
        }

        // 4. TONE: a brighter setting leaves more treble in the tail.
        {
            double prev = -1;
            bool rising = true;
            for (int16_t tk : {0, 50, 100}) {
                Reverb r;
                r.Init(mem.data());
                const int16_t k[4] = {60, 0, tk, 100};
                r.Set(true, uint8_t(model), k);
                std::vector<float> in(48000, 0.0f);
                in[0] = 1.0f;
                auto out = Run(r, in);
                const double ratio = BandEnergy(out, 9600, 4096, 2500, 6000) / BandEnergy(out, 9600, 4096, 200, 600);
                rising = rising && ratio > prev;
                prev = ratio;
            }
            CHECK(rising, "%s: TONE doesn't brighten the tail", name);
        }

        // 5. MIX 0 and never-on are exactly dry; extremes stay bounded.
        {
            auto in = Noise(48000, 0.2f);
            Reverb a, b;
            std::vector<float> mem2(Reverb::kMemFloats);
            a.Init(mem.data());
            b.Init(mem2.data());
            const int16_t k[4] = {80, 30, 50, 0};
            a.Set(true, uint8_t(model), k);
            auto oa = Run(a, in), ob = Run(b, in);
            CHECK(oa == in && ob == in, "%s: MIX 0 / never on is not exactly the dry signal", name);

            Reverb c;
            c.Init(mem.data());
            const int16_t kx[4] = {100, 100, 100, 100};
            c.Set(true, uint8_t(model), kx);
            auto out = Run(c, Noise(48000 * 10, 0.7f));
            CHECK(Finite(out, 20.0f), "%s: blew up at DECAY 100", name);
        }
    }

    // 6. Trails and model switches: the tail rings on after OFF, then the reverb goes idle; a model
    //    change while ringing stays bounded.
    {
        Reverb r;
        r.Init(mem.data());
        int16_t k[4] = {50, 0, 60, 100};
        r.Set(true, Reverb::kHall, k);
        std::vector<float> in(48000 * 12, 0.0f);
        in[100] = 1.0f;
        std::vector<float> out = in;
        for (size_t s = 0; s + kBlock <= out.size(); s += kBlock) {
            if (s == 4800)
                r.Set(false, Reverb::kHall, k);
            r.Process(out.data() + s, kBlock);
        }
        CHECK(Rms(out, 9600, 24000) > 1e-3 && !r.Active(), "Reverb trails: tail after OFF %.5f rms, idle %d", Rms(out, 9600, 24000), !r.Active());

        Reverb m;
        m.Init(mem.data());
        m.Set(true, Reverb::kHall, k);
        auto noise = Noise(48000 * 2, 0.3f);
        for (size_t s = 0; s + kBlock <= noise.size(); s += kBlock) {
            if (s == 48000)
                m.Set(true, Reverb::kSpring, k);
            m.Process(noise.data() + s, kBlock);
        }
        CHECK(Finite(noise, 5.0f), "Reverb: model switch while ringing blew up");
    }
}

static void Timing() {
    std::vector<float> dm(Delay::kMemFloats), rm(Reverb::kMemFloats);
    auto time = [](auto &fx) {
        std::vector<float> x = Noise(kBlock, 0.1f);
        const int blocks = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
            fx.Process(x.data(), kBlock);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
    };
    const int16_t k[4] = {60, 40, 60, 40};
    for (int m = 0; m < 2; ++m) {
        Delay d;
        d.Init(dm.data());
        d.Set(true, uint8_t(m), k);
        printf("  cost on this Mac: Delay %-7s %.2f us per 48-sample block\n", kDelayNames[m], time(d));
    }
    for (int m = 0; m < 4; ++m) {
        Reverb r;
        r.Init(rm.data());
        r.Set(true, uint8_t(m), k);
        printf("  cost on this Mac: Reverb %-6s %.2f us per 48-sample block\n", kReverbNames[m], time(r));
    }
}

int main() {
    TestDelay();
    TestReverb();
    Timing();
    printf("effects: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
