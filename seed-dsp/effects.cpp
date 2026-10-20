#include "effects.h"

#include <cstring>

namespace fx {
namespace {

// Small maths, so the Seed build links no libm (Rocco's cab filter avoids it for the same reason:
// about 13 KB of flash). Accuracy is far beyond what a knob setting needs.
inline float Floor(float x) {
    const int i = static_cast<int>(x);
    return static_cast<float>(x < static_cast<float>(i) ? i - 1 : i);
}

// 2^x, relative error about 1e-4.
inline float Exp2(float x) {
    if (x < -126.0f)
        return 0.0f;
    if (x > 127.0f)
        x = 127.0f;
    const float i = Floor(x), f = x - i;
    const float p = 1.0f + f * (0.69606564f + f * (0.22449434f + f * 0.07944023f));
    union {
        uint32_t u;
        float f;
    } scale{static_cast<uint32_t>(static_cast<int>(i) + 127) << 23};
    return p * scale.f;
}

inline float Sqrt(float x) {
    if (x <= 0.0f)
        return 0.0f;
    union {
        float f;
        uint32_t u;
    } v{x};
    v.u = (v.u >> 1) + 0x1FC00000u; // first guess, then Newton
    float y = v.f;
    for (int i = 0; i < 3; ++i)
        y = 0.5f * (y + x / y);
    return y;
}

// sin(2 pi p) for p in [0, 1), error about 0.001: for the tape's wow and flutter.
inline float Sin2Pi(float p) {
    const float t = 2.0f * (p - 0.5f); // sin(2 pi p) = -sin(pi t), t in [-1, 1)
    const float a = t < 0 ? -t : t;
    float y = 4.0f * t * (1.0f - a);
    y = 0.225f * (y * (y < 0 ? -y : y) - y) + y;
    return -y;
}

// tanh-like saturation, |out| <= 1.
inline float SoftClip(float x) {
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

constexpr float kTwoPi = 6.2831853f;
constexpr float kLog2E = 1.4426950f;
constexpr float kLog2Of10 = 3.3219281f;

// Coefficient of a one-pole low-pass y += (1 - a) (x - y) with its corner at `hz`.
inline float OnePoleA(float hz) { return Exp2(-kTwoPi * hz / kSampleRate * kLog2E); }

// Feedback gain that makes a loop of `samples` fall 60 dB in `rt60` seconds.
inline float LoopGain(float samples, float rt60) { return Exp2(-3.0f * kLog2Of10 * samples / (rt60 * kSampleRate)); }

// Gain at 1 kHz of that one-pole low-pass: a loop's damping is divided out there, so DECAY is the
// decay time at 1 kHz whatever TONE is (TONE only changes how much faster the treble dies).
inline float OnePoleGain1k(float a) { return (1.0f - a) / Sqrt(1.0f - 2.0f * a * 0.99144486f + a * a); }

inline float Min(float a, float b) { return a < b ? a : b; }

inline float Knob(int16_t k) { return static_cast<float>(k < 0 ? 0 : k > 100 ? 100 : k) / 100.0f; }

} // namespace

// ---------------------------------------------------------------------------------------------

void Settings::Set(bool on, uint8_t model, const int16_t knobs[4]) {
    on_.store(on, std::memory_order_relaxed);
    model_.store(model, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        knobs_[i].store(knobs[i], std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_release);
}

bool Settings::Take(bool &on, uint8_t &model, int16_t knobs[4]) {
    const uint32_t g = gen_.load(std::memory_order_acquire);
    if (g == seen_)
        return false;
    seen_ = g;
    on = on_.load(std::memory_order_relaxed);
    model = model_.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
        knobs[i] = knobs_[i].load(std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Delay: one line, a low-pass (and for Tape a low cut and saturation) in the feedback loop. A new
// TIME crossfades from the old read position to the new one, so turning the knob never clicks.

float Delay::TimeMs(int16_t knob) { return 40.0f * Exp2(Knob(knob) * 4.6438562f); } // 40 ms .. 1 s
float Delay::Feedback(int16_t knob) { return 0.9f * Knob(knob); }

void Delay::Init(float *mem) {
    buf_ = mem;
    std::memset(buf_, 0, kMemFloats * sizeof(float));
    w_ = 0;
    delay_target_ = delay_now_ = delay_next_ = 0.1f * kSampleRate;
    xfade_ = 1.0f;
}

void Delay::Apply(bool on, uint8_t model, const int16_t k[4]) {
    if (on && !on_)
        ringing_ = true;
    on_ = on;
    model_ = model == kTape ? kTape : kDigital;
    delay_target_ = TimeMs(k[0]) * kSampleRate / 1000.0f;
    fb_ = Feedback(k[1]);
    // Repeats from dark to bright: 1.5 kHz .. 16 kHz, Tape 0.6 times that.
    float hz = 1500.0f * Exp2(Knob(k[2]) * 3.4150375f);
    if (model_ == kTape)
        hz *= 0.6f;
    lp_a_ = OnePoleA(hz);
    hp_a_ = OnePoleA(150.0f);
    wet_ = Knob(k[3]);
    in_gain_ = on ? 1.0f : 0.0f;
}

// The line `delay` samples back from the write position, linear interpolation.
float Delay::Read(float delay) const {
    float rp = static_cast<float>(w_) - delay;
    if (rp < 0)
        rp += static_cast<float>(kMemFloats);
    const std::size_t i0 = static_cast<std::size_t>(rp);
    const float frac = rp - static_cast<float>(i0);
    const std::size_t i1 = i0 + 1 == kMemFloats ? 0 : i0 + 1;
    return buf_[i0] + frac * (buf_[i1] - buf_[i0]);
}

void Delay::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[4];
    if (settings_.Take(on, model, k))
        Apply(on, model, k);
    if (!on_ && !ringing_)
        return;

    const float fb_step = (fb_ - fb_now_) / n, wet_step = (wet_ - wet_now_) / n, in_step = (in_gain_ - in_now_) / n;
    constexpr float kFadeStep = 1.0f / (0.03f * kSampleRate);
    const bool tape = model_ == kTape;
    for (std::size_t i = 0; i < n; ++i) {
        fb_now_ += fb_step;
        wet_now_ += wet_step;
        in_now_ += in_step;
        if (xfade_ >= 1.0f && delay_target_ != delay_now_) { // start a crossfade to the new TIME
            delay_next_ = delay_target_;
            xfade_ = 0.0f;
        }

        float mod = 0.0f;
        if (tape) { // wow (0.5 Hz, 0.25 ms) and flutter (5.5 Hz, 0.03 ms)
            mod = 12.0f * Sin2Pi(wow_phase_) + 1.5f * Sin2Pi(flutter_phase_);
            wow_phase_ += 0.5f / kSampleRate;
            flutter_phase_ += 5.5f / kSampleRate;
            if (wow_phase_ >= 1.0f)
                wow_phase_ -= 1.0f;
            if (flutter_phase_ >= 1.0f)
                flutter_phase_ -= 1.0f;
        }
        float echo = Read(delay_now_ + mod);
        if (xfade_ < 1.0f) {
            xfade_ += kFadeStep;
            if (xfade_ >= 1.0f) {
                xfade_ = 1.0f;
                delay_now_ = delay_next_;
                echo = Read(delay_now_ + mod);
            } else {
                echo += xfade_ * (Read(delay_next_ + mod) - echo);
            }
        }

        // The loop: tone, (tape: low cut, saturation), feedback.
        lp_z_ += (1.0f - lp_a_) * (echo - lp_z_);
        float loop = lp_z_;
        if (tape) {
            hp_z_ += (1.0f - hp_a_) * (loop - hp_z_);
            loop -= hp_z_;
        }
        float write = x[i] * in_now_ + fb_now_ * loop;
        if (tape)
            write = SoftClip(write);
        buf_[w_] = write;
        if (++w_ == kMemFloats)
            w_ = 0;

        x[i] += wet_now_ * echo;
        quiet_ = (write > 1e-6f || write < -1e-6f) ? 0 : quiet_ + 1;
    }
    fb_now_ = fb_;
    wet_now_ = wet_;
    in_now_ = in_gain_;
    // Off and the whole line has been rewritten with silence: stop until turned on again.
    if (!on_ && quiet_ > kMemFloats)
        ringing_ = false;
}

// ---------------------------------------------------------------------------------------------
// Reverb. Hall, Room and Plate: pre-delay, 8 parallel damped combs, 4 series allpasses (the
// Schroeder / Freeverb network, its lengths scaled per model). Each comb's input is scaled by
// sqrt(1 - g^2), g being its loop gain at 1 kHz, so its impulse response has about unit energy
// there, which keeps MIX at the same loudness whatever DECAY and TONE are. Spring: two loops, each a delay and a chain of stretched allpasses
// (Valimaki, Parker & Abel's spring model): their delay rises steeply towards 4 kHz, so every pass
// round the loop comes back as a chirp, the "drip" of a spring tank.

namespace {
// Freeverb's lengths at 44.1 kHz, scaled to 48 kHz.
constexpr uint32_t kCombLen[Reverb::kCombs] = {1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760};
constexpr uint32_t kApLen[Reverb::kAllpasses] = {605, 480, 371, 245};
constexpr uint32_t kSpringLen[Reverb::kSprings] = {1490, 1913};
constexpr float kMaxScale = 1.25f;
constexpr uint32_t kPreMax = 4800 + 1; // 100 ms
constexpr float kSpringA = 0.6f;
// The allpass chain's delay at 1 kHz, where DECAY is measured: 24 stages of
// K (1 - a^2) / (1 + 2 a cos(K w) + a^2) with K = 6, a = 0.6. It rises to 576 samples at 4 kHz.
constexpr float kSpringChain1k = 41.7f;

struct ModelShape {
    float scale;      // line lengths relative to Freeverb's
    float ap_g;       // allpass diffusion
    float rt_lo, log2_range; // DECAY 0 = rt_lo seconds, 100 = rt_lo * 2^log2_range
    float tone_hi;    // damping corner at TONE 100 (Hz); TONE 0 is 1/8 of it
};
constexpr ModelShape kShapes[4] = {
    {1.25f, 0.5f, 1.0f, 3.0f, 12000.0f},   // Hall: 1 .. 8 s
    {0.55f, 0.5f, 0.25f, 3.0f, 9000.0f},   // Room: 0.25 .. 2 s
    {0.8f, 0.7f, 0.5f, 3.3219281f, 16000.0f}, // Plate: 0.5 .. 5 s, denser and brighter
    {1.0f, 0.0f, 0.8f, 2.3219281f, 5000.0f},  // Spring: 0.8 .. 4 s, dark like a tank
};
} // namespace

float Reverb::DecaySeconds(uint8_t model, int16_t knob) {
    const ModelShape &s = kShapes[model < 4 ? model : 0];
    return s.rt_lo * Exp2(Knob(knob) * s.log2_range);
}
float Reverb::PredelayMs(int16_t knob) { return 100.0f * Knob(knob); }

void Reverb::Init(float *mem) {
    mem_ = mem;
    std::memset(mem_, 0, kMemFloats * sizeof(float));
    model_ = kNoModel;
}

void Reverb::Clear() {
    std::memset(mem_, 0, kMemFloats * sizeof(float));
    for (int c = 0; c < kCombs; ++c)
        comb_lp_[c] = 0;
    for (int s = 0; s < kSprings; ++s) {
        sp_lp_[s] = 0;
        for (int j = 0; j < kSpringStages; ++j)
            for (int p = 0; p < kSpringStretch; ++p)
                sp_x_[s][j][p] = sp_y_[s][j][p] = 0;
    }
    in_hp_z_ = 0;
    quiet_ = 0;
}

void Reverb::Apply(bool on, uint8_t model, const int16_t k[4]) {
    if (model > kSpring)
        model = kHall;
    if (on && !on_)
        ringing_ = true;
    on_ = on;
    const ModelShape &s = kShapes[model];
    if (model != model_) {
        // Lay the lines out in the memory: pre-delay, allpasses, then combs or springs.
        model_ = model;
        Clear();
        float *p = mem_;
        auto place = [&p](Line &l, uint32_t len, uint32_t max) {
            l.buf = p;
            l.len = len;
            l.max = max;
            l.pos = 0;
            p += max;
        };
        place(pre_, 1, kPreMax);
        for (int a = 0; a < kAllpasses; ++a)
            place(ap_[a], static_cast<uint32_t>(kApLen[a] * s.scale), static_cast<uint32_t>(kApLen[a] * kMaxScale) + 1);
        float *combs = p;
        for (int c = 0; c < kCombs; ++c)
            place(comb_[c], static_cast<uint32_t>(kCombLen[c] * s.scale), static_cast<uint32_t>(kCombLen[c] * kMaxScale) + 1);
        p = combs; // the springs reuse the combs' memory
        for (int sp = 0; sp < kSprings; ++sp)
            place(spring_[sp], kSpringLen[sp], kSpringLen[sp] + 1);
        ap_g_ = s.ap_g;
    }
    const float rt60 = DecaySeconds(model, k[0]);
    pre_len_ = static_cast<uint32_t>(PredelayMs(k[1]) * kSampleRate / 1000.0f);
    damp_a_ = OnePoleA(s.tone_hi * Exp2(3.0f * (Knob(k[2]) - 1.0f)));
    in_hp_a_ = OnePoleA(120.0f);
    const float damp_1k = OnePoleGain1k(damp_a_);
    if (model == kSpring) {
        for (int sp = 0; sp < kSprings; ++sp) { // each pass: the loop delay plus the chain's
            sp_g_[sp] = Min(LoopGain(static_cast<float>(kSpringLen[sp]) + kSpringChain1k, rt60) / damp_1k, 0.999f);
            const float eff = sp_g_[sp] * damp_1k; // the loop's gain at 1 kHz, damping included
            sp_in_[sp] = Sqrt(1.0f - eff * eff) * 0.7071068f;
        }
    } else {
        for (int c = 0; c < kCombs; ++c) {
            comb_g_[c] = Min(LoopGain(static_cast<float>(comb_[c].len), rt60) / damp_1k, 0.999f);
            const float eff = comb_g_[c] * damp_1k; // the loop's gain at 1 kHz, damping included
            comb_in_[c] = Sqrt(1.0f - eff * eff) * 0.35355339f; // 1/sqrt(8)
        }
    }
    wet_ = Knob(k[3]);
    in_gain_ = on ? 1.0f : 0.0f;
}

void Reverb::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[4];
    if (settings_.Take(on, model, k))
        Apply(on, model, k);
    if (!on_ && !ringing_)
        return;

    const float wet_step = (wet_ - wet_now_) / n, in_step = (in_gain_ - in_now_) / n;
    const float damp = 1.0f - damp_a_, hp = 1.0f - in_hp_a_;
    for (std::size_t i = 0; i < n; ++i) {
        wet_now_ += wet_step;
        in_now_ += in_step;

        // Low cut into the reverb (keeps the tail from booming), then the pre-delay.
        const float in = x[i] * in_now_;
        in_hp_z_ += hp * (in - in_hp_z_);
        float v = in - in_hp_z_;
        pre_.buf[pre_.pos] = v;
        uint32_t rp = pre_.pos >= pre_len_ ? pre_.pos - pre_len_ : pre_.pos + kPreMax - pre_len_;
        v = pre_.buf[rp];
        if (++pre_.pos == kPreMax)
            pre_.pos = 0;

        float out = 0;
        if (model_ == kSpring) {
            const uint32_t ph = sp_phase_;
            for (int s = 0; s < kSprings; ++s) {
                Line &l = spring_[s];
                const float back = l.buf[l.pos];
                sp_lp_[s] += damp * (back - sp_lp_[s]);
                float u = v * sp_in_[s] + sp_g_[s] * sp_lp_[s];
                for (int j = 0; j < kSpringStages; ++j) { // y[n] = a (u[n] - y[n-K]) + u[n-K]
                    const float y = kSpringA * (u - sp_y_[s][j][ph]) + sp_x_[s][j][ph];
                    sp_x_[s][j][ph] = u;
                    sp_y_[s][j][ph] = y;
                    u = y;
                }
                l.buf[l.pos] = u;
                if (++l.pos == l.len)
                    l.pos = 0;
                out += back;
            }
            if (++sp_phase_ == kSpringStretch)
                sp_phase_ = 0;
        } else {
            for (int c = 0; c < kCombs; ++c) {
                Line &l = comb_[c];
                const float y = l.buf[l.pos];
                comb_lp_[c] += damp * (y - comb_lp_[c]);
                l.buf[l.pos] = v * comb_in_[c] + comb_g_[c] * comb_lp_[c];
                if (++l.pos == l.len)
                    l.pos = 0;
                out += y;
            }
            for (int a = 0; a < kAllpasses; ++a) {
                Line &l = ap_[a];
                const float d = l.buf[l.pos];
                const float w = out + ap_g_ * d;
                l.buf[l.pos] = w;
                if (++l.pos == l.len)
                    l.pos = 0;
                out = d - ap_g_ * w;
            }
        }
        x[i] += wet_now_ * out;
        quiet_ = (out > 1e-6f || out < -1e-6f || v > 1e-6f || v < -1e-6f) ? 0 : quiet_ + 1;
    }
    wet_now_ = wet_;
    in_now_ = in_gain_;
    // Off and silent for longer than the longest loop takes to empty: stop until turned on.
    if (!on_ && quiet_ > kSampleRate)
        ringing_ = false;
}

} // namespace fx
