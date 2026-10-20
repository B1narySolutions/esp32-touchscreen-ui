// Host tests for effects: delay (echo times, repeat levels), reverb (measured decay times, level,
// pre-delay, tone, trails), gate (closing, hysteresis), compressor (static curve, attack), drive
// (harmonics, aliasing, level), chorus (base delay, level, no clicks), the rig (chain order, link
// ids); exact dry signal when off, and no blow-ups at extreme settings.  Build and run: bash seed-dsp/test.sh
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

// RT60 of an impulse response in an octave band (DECAY is the decay of the lows and mids; the
// damping makes the treble die sooner): Schroeder backward integration, slope from -5 to -25 dB.
static double Rt60(const std::vector<float> &raw, double band_hz = 500.0) {
    // RBJ band-pass, one octave wide.
    const double w = 2 * M_PI * band_hz / 48000.0, alpha = std::sin(w) * std::sinh(std::log(2.0) / 2 * 1.0 * w / std::sin(w));
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
            printf("  %-6s DECAY %3d: RT60 %.2f s at 500 Hz (set %.2f s)\n", name, dk, got, want);
        }

        // 1b. A dark TONE never makes the lows ring longer than DECAY (it only shortens the treble),
        //     and after OFF the tail dies and the effect goes idle (the touchscreen's defaults, and
        //     TONE 0, with noise in).
        for (int16_t tk : {0, 45}) {
            Reverb r;
            r.Init(mem.data());
            const int16_t k[4] = {35, 5, tk, 100};
            r.Set(true, uint8_t(model), k);
            std::vector<float> in(size_t(48000 * 8), 0.0f);
            in[0] = 1.0f;
            auto h = Run(r, in);
            h[0] = 0;
            const double want = Reverb::DecaySeconds(uint8_t(model), 35), low = Rt60(h, 250.0);
            CHECK(low < want * 1.15, "%s TONE %d: lows ring %.2f s, DECAY is %.2f s", name, tk, low, want);

            Reverb q;
            q.Init(mem.data());
            q.Set(true, uint8_t(model), k);
            auto noise = Noise(48000 * 2, 0.3f);
            Run(q, noise);
            q.Set(false, uint8_t(model), k);
            std::vector<float> silence(size_t(48000 * (want * 3 + 3)), 0.0f);
            Run(q, silence);
            CHECK(!q.Active(), "%s TONE %d: still ringing %.0f s after OFF", name, tk, want * 3 + 3);
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

// Power at a frequency (Goertzel), for whole cycles: amplitude^2 / 2 for a sine.
static double ToneEnergy(const std::vector<float> &v, size_t from, size_t len, double hz) {
    const double w = 2 * M_PI * hz / 48000.0, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        const double s0 = v[from + i] + c * s1 - s2;
        s2 = s1, s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2 * (re * re + im * im) / (double(len) * len);
}

static std::vector<float> Sine(size_t n, double hz, float amp) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = amp * float(std::sin(2 * M_PI * hz * double(i) / 48000.0));
    return v;
}

static double PeakDb(const std::vector<float> &v, size_t from, size_t to) {
    double p = 0;
    for (size_t i = from; i < to; ++i)
        p = std::max(p, double(std::fabs(v[i])));
    return 20 * std::log10(p + 1e-12);
}

static void TestGate() {
    CHECK(Gate::ThresholdDb(0) == -80 && Gate::ThresholdDb(100) == -20 && Gate::RangeDb(100) == 90, "gate mappings");
    const int16_t k[4] = {35, 20, 40, 70}; // the touchscreen's defaults: -59 dBFS, range 63 dB
    {
        Gate g;
        g.Set(true, 0, k);
        auto in = Sine(48000, 220, 0.1f); // -20 dBFS peak: well above
        auto out = Run(g, in);
        const double d = PeakDb(out, 24000, 48000) - PeakDb(in, 24000, 48000);
        CHECK(std::fabs(d) < 0.1, "gate: a signal above THRESHOLD changed by %.2f dB", d);
    }
    {
        Gate g;
        g.Set(true, 0, k);
        auto in = Sine(48000, 220, 0.0002f); // -74 dBFS: below
        auto out = Run(g, in);
        const double d = PeakDb(out, 24000, 48000) - PeakDb(in, 24000, 48000);
        CHECK(std::fabs(d + Gate::RangeDb(70)) < 1.0, "gate: below THRESHOLD turned down %.1f dB (RANGE %.0f dB)", -d, Gate::RangeDb(70));
        printf("  Gate: -74 dBFS hum turned down %.1f dB\n", -d);
    }
    {
        // Hysteresis: after a note, a tail between the close and open levels keeps it open.
        Gate g;
        g.Set(true, 0, k);
        auto in = Sine(48000, 220, 0.1f);
        for (size_t i = 12000; i < in.size(); ++i)
            in[i] *= 0.0085f; // -61 dB: below the -59 dB open level, above the -65 dB close level
        auto out = Run(g, in);
        const double d = PeakDb(out, 36000, 48000) - PeakDb(in, 36000, 48000);
        CHECK(std::fabs(d) < 0.5, "gate: a tail inside the hysteresis band was cut by %.1f dB", -d);
    }
    {
        auto in = Noise(48000, 0.1f);
        Gate g;
        auto out = Run(g, in);
        CHECK(out == in, "gate: never on is not exactly the dry signal");
    }
}

static void TestCompressor() {
    CHECK(std::fabs(Compressor::Ratio(0) - 1) < 1e-3 && std::fabs(Compressor::Ratio(100) - 20) < 0.05, "comp: RATIO 1 .. 20");
    // Static curve: steady sines at several levels (fast attack, slow release, so the detector holds
    // the peak); the output peak follows the curve.
    const int16_t k[5] = {50, 46, 0, 70, 33}; // -30 dBFS, about 4:1, 0.1 ms, 0.31 s, about 0 dB
    const float thr = Compressor::ThresholdDb(50), ratio = Compressor::Ratio(46), makeup = Compressor::MakeupDb(33);
    double worst = 0;
    for (float level_db : {-45.0f, -30.0f, -20.0f, -10.0f}) {
        Compressor c;
        c.Set(true, 0, k);
        auto in = Sine(48000, 220, float(std::pow(10.0, level_db / 20)));
        auto out = Run(c, in);
        const double got = PeakDb(out, 36000, 48000), want = Compressor::CurveDb(level_db, thr, ratio) + makeup;
        worst = std::max(worst, std::fabs(got - want));
        CHECK(std::fabs(got - want) < 1.0, "comp: %.0f dBFS in -> %.1f dBFS out, curve says %.1f", level_db, got, want);
    }
    printf("  Comp: static curve within %.2f dB (threshold %.0f dBFS, %.1f:1)\n", worst, thr, ratio);
    {
        // ATTACK: a jump from -40 to -10 dBFS is only partly turned down after 1 ms at 10 ms attack.
        const int16_t ks[5] = {50, 46, 50, 70, 33};
        Compressor c;
        c.Set(true, 0, ks);
        auto in = Sine(48000, 1000, 0.01f);
        for (size_t i = 24000; i < in.size(); ++i)
            in[i] *= 31.6f;
        auto out = Run(c, in);
        const double early = PeakDb(out, 24000 + 48, 24000 + 96), late = PeakDb(out, 40000, 48000);
        CHECK(early > late + 2, "comp: ATTACK 10 ms, but 1 ms after the jump it's already at %.1f dB (settles at %.1f)", early, late);
    }
    {
        auto in = Noise(48000, 0.1f);
        Compressor c;
        auto out = Run(c, in);
        CHECK(out == in, "comp: never on is not exactly the dry signal");
    }
}

static const char *kDriveNames[] = {"Tube Screamer", "Big Muff", "Klon-style", "Rat"};

// In-place radix-2 FFT (n a power of 2), for the aliasing measurement.
static void Fft(std::vector<double> &re, std::vector<double> &im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(re[i], re[j]), std::swap(im[i], im[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double a = -2 * M_PI / double(len);
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const double wr = std::cos(a * double(k)), wi = std::sin(a * double(k));
                const double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr, im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr, im[i + k] += xi;
            }
    }
}

// Harmonic distortion of a 220 Hz sine; and the audible aliasing of a 996 Hz sine (a whole number
// of cycles in 32768 samples): the energy below 16 kHz that isn't at a harmonic, relative to the
// output. Folding into 18-24 kHz is left out: inaudible, and the cabinet removes it.
static void DriveSpectrum(int model, int16_t gain, double &thd_db, double &alias_db) {
    {
        Drive d;
        const int16_t k[3] = {gain, 50, 50};
        d.Set(true, uint8_t(model), k);
        auto out = Run(d, Sine(48000 * 2, 220, 0.1f));
        const size_t from = 48000, len = 48000;
        const double f1 = ToneEnergy(out, from, len, 220);
        double h = 0;
        for (int n = 2; n * 220 < 20000; ++n)
            h += ToneEnergy(out, from, len, 220.0 * n);
        thd_db = 10 * std::log10(h / f1 + 1e-30);
    }
    {
        constexpr size_t kN = 32768, kBin = 680; // 996.1 Hz
        Drive d;
        const int16_t k[3] = {gain, 100, 50};
        d.Set(true, uint8_t(model), k);
        auto out = Run(d, Sine(48000 + kN + kBlock, 48000.0 * kBin / kN, 0.1f));
        std::vector<double> re(kN), im(kN, 0.0);
        for (size_t i = 0; i < kN; ++i)
            re[i] = out[48000 + i];
        Fft(re, im);
        double total = 0, stray = 0;
        for (size_t b = 1; b < kN / 2; ++b) {
            const double p = re[b] * re[b] + im[b] * im[b];
            total += p;
            if (b % kBin && double(b) * 48000.0 / kN < 16000.0)
                stray += p;
        }
        alias_db = 10 * std::log10(stray / total + 1e-30);
    }
}

static void TestDrive() {
    for (int model = 0; model < 4; ++model) {
        const char *name = kDriveNames[model];
        double prev = -1e9, thd = 0, alias = 0;
        bool rising = true;
        for (int16_t g : {0, 50, 100}) {
            DriveSpectrum(model, g, thd, alias);
            rising = rising && thd > prev;
            prev = thd;
            CHECK(alias < -60, "%s GAIN %d: aliasing at %.1f dB", name, g, alias);
            printf("  %-13s GAIN %3d: harmonics %+6.1f dB, aliasing %6.1f dB\n", name, g, thd, alias);
        }
        CHECK(rising, "%s: more GAIN doesn't add harmonics", name);

        // LEVEL is the output level: +10 dB from 50 to 75.
        double rms[2];
        for (int i = 0; i < 2; ++i) {
            Drive d;
            const int16_t k[3] = {50, 50, int16_t(i ? 75 : 50)};
            d.Set(true, uint8_t(model), k);
            auto out = Run(d, GuitarNoise(48000, 0.1f));
            rms[i] = Rms(out, 24000, 48000);
        }
        const double step = 20 * std::log10(rms[1] / rms[0]);
        CHECK(std::fabs(step - 10) < 0.3, "%s: LEVEL 50 -> 75 changed the output %.2f dB (expected +10)", name, step);

        Drive d;
        const int16_t kx[3] = {100, 100, 100};
        d.Set(true, uint8_t(model), kx);
        auto out = Run(d, Noise(48000 * 2, 1.0f));
        // The clean signal is added back (Tube Screamer, Klon), so peaks follow the noise's.
        CHECK(Finite(out, 20.0f), "%s: blew up with loud noise at GAIN 100", name);
    }
    auto in = Noise(48000, 0.1f);
    Drive d;
    auto out = Run(d, in);
    CHECK(out == in, "drive: never on is not exactly the dry signal");
}

static void TestChorus() {
    std::vector<float> mem(Chorus::kMemFloats);
    static const char *names[] = {"Classic", "Dimension", "Ensemble"};
    for (int model = 0; model < 3; ++model) {
        {
            // DEPTH 0, MIX 100: a pure delay of the base time (the voices all identical then).
            Chorus c;
            c.Init(mem.data());
            const int16_t k[3] = {50, 0, 100};
            c.Set(true, uint8_t(model), k);
            auto in = Noise(48000, 0.1f);
            auto out = Run(c, in);
            const size_t base = model == 0 ? 576 : model == 1 ? 288 : 480;
            const float gain = model == 0 ? 1.0f : model == 1 ? 2 * 0.70710678f : 3 * 0.5f;
            double err = 0;
            for (size_t i = 2400; i < in.size(); ++i)
                err = std::max(err, double(std::fabs(out[i] - gain * in[i - base])));
            CHECK(err < 1e-5, "%s: DEPTH 0 isn't a clean %.0f ms delay (error %.2g)", names[model], base / 48.0, err);
        }
        {
            // DEPTH moves the pitch (the output isn't the delayed input any more) at the same level,
            // and changing RATE mid-note doesn't click.
            Chorus c;
            c.Init(mem.data());
            int16_t k[3] = {40, 80, 50};
            c.Set(true, uint8_t(model), k);
            auto in = Sine(48000 * 3, 330, 0.3f);
            std::vector<float> out = in;
            double before = 0, after = 0;
            for (size_t s = 0; s + kBlock <= out.size(); s += kBlock) {
                if (s == 48000 * 2) {
                    k[0] = 90;
                    c.Set(true, uint8_t(model), k);
                }
                c.Process(out.data() + s, kBlock);
                for (size_t i = s ? s : 1; i < s + kBlock; ++i)
                    if (s >= 9600)
                        (s < 48000 * 2 ? before : after) = std::max(s < 48000 * 2 ? before : after, double(std::fabs(out[i] - out[i - 1])));
            }
            CHECK(after < 1.5 * before, "%s: a RATE change made a step of %.3f (steady %.3f)", names[model], after, before);
        }
        for (int16_t mix : {50, 100}) {
            // About the same loudness at any MIX (guitar-like noise).
            Chorus c;
            c.Init(mem.data());
            const int16_t k[3] = {40, 80, mix};
            c.Set(true, uint8_t(model), k);
            auto in = GuitarNoise(48000 * 2, 0.1f);
            auto out = Run(c, in);
            const double level = 20 * std::log10(Rms(out, 48000, 96000) / Rms(in, 48000, 96000));
            CHECK(std::fabs(level) < 1.5, "%s: MIX %d changed the level by %.1f dB", names[model], mix, level);
            printf("  Chorus %-9s MIX %3d: level %+.1f dB\n", names[model], mix, level);
        }
        {
            auto in = Noise(48000, 0.2f);
            Chorus c;
            c.Init(mem.data());
            const int16_t k[3] = {50, 60, 0};
            c.Set(true, uint8_t(model), k);
            auto out = Run(c, in);
            CHECK(out == in, "%s: MIX 0 is not exactly the dry signal", names[model]);
        }
    }
}

static void TestRig() {
    std::vector<float> dm(Delay::kMemFloats), rm(Reverb::kMemFloats), cm(Chorus::kMemFloats);
    auto impulse = [] {
        std::vector<float> v(48000, 0.0f);
        v[100] = 1.0f;
        return v;
    };
    // A delay turned on through the link's ids: after the amp by default, before it when the chain
    // puts it there.
    for (bool before : {false, true}) {
        Rig r;
        r.Init(dm.data(), rm.data(), cm.data());
        r.SetFx(Rig::kDelay, true, 0);
        const uint16_t base = uint16_t(Rig::kDelay) << 8;
        const int16_t vals[4] = {30, 0, 100, 100};
        for (uint16_t i = 0; i < 4; ++i)
            r.SetParam(uint16_t(base | i), vals[i]);
        r.SetParam(0x0C00, 50); // the cabinet's: not run by Rig, ignored
        if (before) {
            const uint8_t order[] = {Rig::kDelay, Rig::kGate, Rig::kAmp, Rig::kReverb};
            r.SetChain(order, 4);
        }
        auto pre = impulse(), post = impulse();
        for (size_t s = 0; s + kBlock <= pre.size(); s += kBlock)
            r.ProcessPre(pre.data() + s, kBlock);
        for (size_t s = 0; s + kBlock <= post.size(); s += kBlock)
            r.ProcessPost(post.data() + s, kBlock);
        const size_t at = 100 + size_t(Delay::TimeMs(30) * 48.0); // the echo falls between two samples
        const bool echo_pre = PeakDb(pre, at - 2, at + 3) > -6, echo_post = PeakDb(post, at - 2, at + 3) > -6;
        CHECK(echo_pre == before && echo_post == !before && r.BeforeAmp(Rig::kDelay) == before,
              "rig: delay %s the amp, echo in pre %d, in post %d", before ? "before" : "after", echo_pre, echo_post);
    }
    CHECK(Rig::Runs(Rig::kGate) && Rig::Runs(Rig::kReverb) && !Rig::Runs(3) && !Rig::Runs(Rig::kAmp), "rig: which effects it runs");
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
    {
        Gate g;
        g.Set(true, 0, k);
        printf("  cost on this Mac: Gate          %.2f us per 48-sample block\n", time(g));
        const int16_t k5[5] = {60, 40, 60, 40, 40};
        Compressor c;
        c.Set(true, 0, k5);
        printf("  cost on this Mac: Compressor    %.2f us per 48-sample block\n", time(c));
    }
    for (int m = 0; m < 4; ++m) {
        Drive d;
        d.Set(true, uint8_t(m), k);
        printf("  cost on this Mac: Drive %-13s %.2f us per 48-sample block\n", kDriveNames[m], time(d));
    }
    std::vector<float> cm(Chorus::kMemFloats);
    static const char *cn[] = {"Classic", "Dimension", "Ensemble"};
    for (int m = 0; m < 3; ++m) {
        Chorus c;
        c.Init(cm.data());
        c.Set(true, uint8_t(m), k);
        printf("  cost on this Mac: Chorus %-9s %.2f us per 48-sample block\n", cn[m], time(c));
    }
}

int main() {
    TestDelay();
    TestReverb();
    TestGate();
    TestCompressor();
    TestDrive();
    TestChorus();
    TestRig();
    Timing();
    printf("effects: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
