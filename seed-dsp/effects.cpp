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

// log2(x) for x > 0, error about 1e-4.
inline float Log2(float x) {
    if (x <= 1e-30f)
        return -100.0f;
    union {
        float f;
        uint32_t u;
    } v{x};
    const float e = static_cast<float>(static_cast<int>((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; // mantissa in [1, 2)
    const float m = v.f;
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
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

// Gain at 500 Hz of that one-pole low-pass, for the reverb's level normalisation.
inline float OnePoleGain500(float a) { return (1.0f - a) / Sqrt(1.0f - 2.0f * a * 0.99785892f + a * a); }

inline float Knob(int16_t k) { return static_cast<float>(k < 0 ? 0 : k > 100 ? 100 : k) / 100.0f; }

} // namespace

// ---------------------------------------------------------------------------------------------

void Settings::Set(bool on, uint8_t model, const int16_t *knobs, int count) {
    on_.store(on, std::memory_order_relaxed);
    model_.store(model, std::memory_order_relaxed);
    for (int i = 0; i < kMaxKnobs; ++i)
        knobs_[i].store(i < count ? knobs[i] : int16_t(0), std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_release);
}

bool Settings::Take(bool &on, uint8_t &model, int16_t knobs[kMaxKnobs]) {
    const uint32_t g = gen_.load(std::memory_order_acquire);
    if (g == seen_)
        return false;
    seen_ = g;
    on = on_.load(std::memory_order_relaxed);
    model = model_.load(std::memory_order_relaxed);
    for (int i = 0; i < kMaxKnobs; ++i)
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

void Delay::Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]) {
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
    int16_t k[kMaxKnobs];
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
// sqrt(1 - g^2), g being its loop gain at 500 Hz, so its impulse response has about unit energy
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
// The allpass chain's delay at low frequencies: 24 stages of K (1 - a^2) / (1 + 2 a cos(K w) + a^2)
// with K = 6, a = 0.6 is 36 samples at 100 Hz and 42 at 1 kHz; it rises to 576 at 4 kHz.
constexpr float kSpringChainLow = 37.0f;

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

void Reverb::Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]) {
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
    // DECAY is the decay time of the lows and mids (the loop gain at DC); TONE's damping makes the
    // treble die sooner on top of it, as in a real room or tank. The input of each loop is scaled
    // by its gain at 500 Hz so MIX keeps about the same loudness whatever DECAY and TONE are.
    const float damp_500 = OnePoleGain500(damp_a_);
    if (model == kSpring) {
        for (int sp = 0; sp < kSprings; ++sp) { // each pass: the loop delay plus the chain's
            sp_g_[sp] = LoopGain(static_cast<float>(kSpringLen[sp]) + kSpringChainLow, rt60);
            const float eff = sp_g_[sp] * damp_500;
            sp_in_[sp] = Sqrt(1.0f - eff * eff) * 0.7071068f;
        }
    } else {
        for (int c = 0; c < kCombs; ++c) {
            comb_g_[c] = LoopGain(static_cast<float>(comb_[c].len), rt60);
            const float eff = comb_g_[c] * damp_500;
            comb_in_[c] = Sqrt(1.0f - eff * eff) * 0.35355339f; // 1/sqrt(8)
        }
    }
    wet_ = Knob(k[3]);
    in_gain_ = on ? 1.0f : 0.0f;
}

void Reverb::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[kMaxKnobs];
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

// ---------------------------------------------------------------------------------------------
// Pedals before the amp, and the chorus. Each switches in and out with a 5 ms crossfade.

namespace {
constexpr float kFadeStep = 1.0f / (0.005f * kSampleRate);
constexpr float kDbToLog2 = 0.16609640f; // log2(10) / 20
constexpr float kLog2ToDb = 6.0205999f;  // 20 log10(2)

inline float DbToLin(float db) { return Exp2(db * kDbToLog2); }
inline float LinToDb(float x) { return kLog2ToDb * Log2(x); }
inline float Abs(float x) { return x < 0 ? -x : x; }
// Coefficient of a one-pole smoother y += c (x - y) with time constant `seconds`.
inline float TimeCoef(float seconds) { return 1.0f - Exp2(-kLog2E / (seconds * kSampleRate)); }
// One-pole low-pass coefficient (as OnePoleA) at 96 kHz, for the oversampled drive.
inline float OnePoleA2x(float hz) { return Exp2(-kTwoPi * hz / (2.0f * kSampleRate) * kLog2E); }
inline void FadeTowards(float &fade, bool on) {
    fade += on ? kFadeStep : -kFadeStep;
    fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
}
} // namespace

// Gate --------------------------------------------------------------------------------------------

float Gate::ThresholdDb(int16_t knob) { return -80.0f + 60.0f * Knob(knob); }
float Gate::RangeDb(int16_t knob) { return 90.0f * Knob(knob); }

void Gate::Apply(bool on, const int16_t k[kMaxKnobs]) {
    on_ = on;
    open_ = DbToLin(ThresholdDb(k[0]));
    close_ = 0.5f * open_;                                   // 6 dB of hysteresis
    att_ = TimeCoef(0.0001f * Exp2(Knob(k[1]) * 7.6438562f)); // 0.1 .. 20 ms
    rel_ = TimeCoef(0.01f * Exp2(Knob(k[2]) * 6.6438562f));  // 10 ms .. 1 s
    floor_ = DbToLin(-RangeDb(k[3]));
}

void Gate::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[kMaxKnobs];
    if (settings_.Take(on, model, k))
        Apply(on, k);
    if (!on_ && fade_ <= 0)
        return;
    constexpr float kEnvDecay = 0.99584202f; // the detector falls with a 5 ms time constant
    constexpr uint32_t kHold = 480;          // stays open 10 ms after the level drops
    for (std::size_t i = 0; i < n; ++i) {
        const float a = Abs(x[i]);
        env_ = a > env_ ? a : env_ * kEnvDecay;
        if (env_ > open_) {
            is_open_ = true;
            hold_ = kHold;
        } else if (env_ < close_) {
            if (hold_)
                --hold_;
            else
                is_open_ = false;
        }
        const float target = is_open_ ? 1.0f : floor_;
        if (!started_) {
            gain_ = target;
            started_ = true;
        }
        gain_ += (target - gain_) * (target > gain_ ? att_ : rel_);
        FadeTowards(fade_, on_);
        x[i] *= 1.0f + fade_ * (gain_ - 1.0f);
    }
}

// Compressor ---------------------------------------------------------------------------------------

float Compressor::ThresholdDb(int16_t knob) { return -60.0f + 60.0f * Knob(knob); }
float Compressor::Ratio(int16_t knob) { return Exp2(Knob(knob) * 4.3219281f); } // 1 .. 20
float Compressor::MakeupDb(int16_t knob) { return -12.0f + 36.0f * Knob(knob); }

float Compressor::CurveDb(float in_db, float threshold_db, float ratio) {
    constexpr float kKnee = 6.0f;
    const float over = in_db - threshold_db;
    if (2.0f * over <= -kKnee)
        return in_db;
    if (2.0f * over >= kKnee)
        return threshold_db + over / ratio;
    const float t = over + 0.5f * kKnee;
    return in_db + (1.0f / ratio - 1.0f) * t * t / (2.0f * kKnee);
}

void Compressor::Apply(bool on, const int16_t k[kMaxKnobs]) {
    on_ = on;
    thr_ = ThresholdDb(k[0]);
    ratio_ = Ratio(k[1]);
    att_ = TimeCoef(0.0001f * Exp2(Knob(k[2]) * 9.9657843f)); // 0.1 .. 100 ms
    rel_ = TimeCoef(0.02f * Exp2(Knob(k[3]) * 5.6438562f));   // 20 ms .. 1 s
    makeup_db_ = MakeupDb(k[4]);
}

void Compressor::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[kMaxKnobs];
    if (settings_.Take(on, model, k))
        Apply(on, k);
    if (!on_ && fade_ <= 0)
        return;
    for (std::size_t i = 0; i < n; ++i) {
        // Peak level, rising with ATTACK and falling with RELEASE, through the static curve.
        const float a = Abs(x[i]);
        level_ += (a - level_) * (a > level_ ? att_ : rel_);
        const float level_db = LinToDb(level_ + 1e-9f);
        const float change_db = CurveDb(level_db, thr_, ratio_) - level_db;
        reduction_db_ = change_db;
        FadeTowards(fade_, on_);
        const float gain = DbToLin(change_db + makeup_db_);
        x[i] *= 1.0f + fade_ * (gain - 1.0f);
    }
}

// HalfBand2x --------------------------------------------------------------------------------------

// Taps at odd lags 1, 3, .. (the even lags are 0, the centre tap is 0.5); least-squares fits.
template <> const float HalfBand2x<12>::kCoefs[12] = {
    0.315846914f, -0.098916804f, 0.052326376f, -0.030843390f, 0.018451693f, -0.010754776f,
    0.005948113f, -0.003048013f, 0.001406671f, -0.000560437f, 0.000178141f, -0.000036737f};
template <> const float HalfBand2x<5>::kCoefs[5] = {0.307483694f, -0.077303240f, 0.025568225f, -0.006754463f, 0.001016715f};

template <int Pairs> void HalfBand2x<Pairs>::Up(float x, float &first, float &second) {
    constexpr int kHist = 2 * Pairs, M = Pairs;
    up_[up_pos_] = x;
    up_[up_pos_ + kHist] = x;
    const float *h = &up_[up_pos_ + kHist]; // h[-t] is t samples ago
    if (++up_pos_ == kHist)
        up_pos_ = 0;
    // Between the samples M and M-1 ago, then the sample M-1 ago itself (zero-stuffing gain 2).
    float acc = 0;
    for (int k = 1; k <= M; ++k)
        acc += kCoefs[k - 1] * (h[-(M - k)] + h[-(M - 1 + k)]);
    first = 2.0f * acc;
    second = h[-(M - 1)];
}

template <int Pairs> float HalfBand2x<Pairs>::Down(float first, float second) {
    constexpr int kHist = 2 * Pairs, M = Pairs;
    down_odd_[down_pos_] = first;
    down_odd_[down_pos_ + kHist] = first;
    down_even_[down_pos_] = second;
    down_even_[down_pos_ + kHist] = second;
    const float *b = &down_odd_[down_pos_ + kHist], *e = &down_even_[down_pos_ + kHist];
    if (++down_pos_ == kHist)
        down_pos_ = 0;
    float acc = 0.5f * e[-M];
    for (int k = 1; k <= M; ++k)
        acc += kCoefs[k - 1] * (b[-(M - k)] + b[-(M + k - 1)]);
    return acc;
}

template class HalfBand2x<12>;
template class HalfBand2x<5>;

// Drive --------------------------------------------------------------------------------------------

namespace {
// One-pole low-pass coefficient (as OnePoleA) at 4x the sample rate.
inline float OnePoleA4x(float hz) { return Exp2(-kTwoPi * hz / (4.0f * kSampleRate) * kLog2E); }
} // namespace

float Drive::GainDb(uint8_t model, int16_t knob) {
    const float k = Knob(knob);
    switch (model) {
    case kMuff: return 10.0f + 30.0f * k;     // first stage; the second adds a fixed 20 dB
    case kKlon: return 32.0f * k;
    case kRat: return 66.0f * k;
    default: return 12.0f + 29.0f * k;        // Tube Screamer: 12 .. 41 dB
    }
}

void Drive::Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]) {
    on_ = on;
    model_ = model <= kRat ? model : kScreamer;
    gain_ = DbToLin(GainDb(model_, k[0]));
    level_ = DbToLin(-30.0f + 40.0f * Knob(k[2]));
    const float tone = Knob(k[1]);
    gain2_ = 1.0f;
    clean_ = 0.0f;
    switch (model_) {
    case kScreamer:
        in_hp_a_ = OnePoleA2x(720.0f); // only the mids and up get clipped: the TS hump
        clip_ = 0.5f;
        clean_ = 1.0f;                 // the op-amp passes the clean signal too
        tone_lp_a_ = OnePoleA(700.0f * Exp2(tone * 3.0995356f)); // 700 Hz .. 6 kHz
        break;
    case kMuff:
        in_hp_a_ = OnePoleA2x(80.0f);
        mid_hp_a_ = OnePoleA4x(100.0f);
        gain2_ = DbToLin(20.0f);
        clip_ = 0.6f;
        tone_lp_a_ = OnePoleA(400.0f); // tone blends these two: a mid scoop in between
        tone_hp_a_ = OnePoleA(1200.0f);
        tone_ = tone;
        break;
    case kKlon:
        in_hp_a_ = OnePoleA2x(80.0f);
        clip_ = 0.4f;
        clean_ = 1.0f - 0.8f * Knob(k[0]); // more clean signal at low gain: "transparent"
        tone_lp_a_ = OnePoleA(1500.0f);    // tone: cut or boost above 1.5 kHz
        tone_ = -0.7f + 1.7f * tone;
        break;
    default: { // Rat
        in_hp_a_ = OnePoleA2x(60.0f);
        const float bw = 1.0e6f / gain_; // gain-bandwidth limit
        bw_a_ = OnePoleA4x(bw < 20000.0f ? bw : 20000.0f);
        clip_ = 0.6f;
        tone_lp_a_ = OnePoleA(600.0f * Exp2(tone * 4.3219281f)); // 600 Hz .. 12 kHz
        break;
    }
    }
}

namespace {
// The clipper: f(x) = x - x^3 / 6.75 up to |x| = 1.5, then +-1; and its antiderivative.
inline float ClipCurve(float x) {
    if (x >= 1.5f)
        return 1.0f;
    if (x <= -1.5f)
        return -1.0f;
    return x - x * x * x * (1.0f / 6.75f);
}
inline float ClipIntegral(float x) {
    const float a = x < 0 ? -x : x;
    if (a >= 1.5f)
        return a - 0.5625f;
    const float x2 = x * x;
    return 0.5f * x2 - x2 * x2 * (1.0f / 27.0f);
}
} // namespace

float Drive::Clip(float x, float &prev) {
    const float x0 = prev;
    prev = x;
    // Both on the same flat part: exactly +-1 (and no cancellation in the difference below).
    if ((x >= 1.5f && x0 >= 1.5f) || (x <= -1.5f && x0 <= -1.5f))
        return x > 0 ? 1.0f : -1.0f;
    const float dx = x - x0;
    if (dx > -1e-4f && dx < 1e-4f)
        return ClipCurve(0.5f * (x + x0));
    return (ClipIntegral(x) - ClipIntegral(x0)) / dx;
}

float Drive::Shape(float s) {
    switch (model_) {
    case kMuff: {
        const float u = Clip(gain_ * s, clip_prev_[0]);
        mid_hp_z_ += (1.0f - mid_hp_a_) * (u - mid_hp_z_);
        return Clip(gain2_ * (u - mid_hp_z_), clip_prev_[1]);
    }
    case kRat:
        bw_z_ += (1.0f - bw_a_) * (gain_ * s - bw_z_);
        return Clip(bw_z_, clip_prev_[0]);
    default:
        return Clip(gain_ * s, clip_prev_[0]);
    }
}

float Drive::Stage(float r) {
    clean_hist_[clean_pos_] = r;
    const float clean = clean_hist_[(clean_pos_ + 16 - HalfBand2x<5>::kLatency) & 15];
    clean_pos_ = (clean_pos_ + 1) & 15;
    in_hp_z_ += (1.0f - in_hp_a_) * (r - in_hp_z_);
    float a, b;
    inner_.Up(r - in_hp_z_, a, b);
    a = Shape(a);
    b = Shape(b);
    return clip_ * inner_.Down(a, b) + clean_ * clean;
}

void Drive::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[kMaxKnobs];
    if (settings_.Take(on, model, k))
        Apply(on, model, k);
    if (!on_ && fade_ <= 0)
        return;
    for (std::size_t i = 0; i < n; ++i) {
        const float in = x[i];
        float a, b;
        outer_.Up(in, a, b);
        a = Stage(a);
        b = Stage(b);
        float y = outer_.Down(a, b);

        tone_lp_z_ += (1.0f - tone_lp_a_) * (y - tone_lp_z_);
        switch (model_) {
        case kMuff:
            tone_hp_z_ += (1.0f - tone_hp_a_) * (y - tone_hp_z_);
            y = (1.0f - tone_) * tone_lp_z_ + tone_ * (y - tone_hp_z_);
            break;
        case kKlon:
            y += tone_ * (y - tone_lp_z_);
            break;
        default:
            y = tone_lp_z_;
            break;
        }
        y *= level_;
        FadeTowards(fade_, on_);
        x[i] = in + fade_ * (y - in);
    }
}

// Chorus -------------------------------------------------------------------------------------------

namespace {
struct ChorusShape {
    float base_ms, max_depth_ms, rate_lo, log2_rate_range;
};
constexpr ChorusShape kChorusShapes[3] = {
    {12.0f, 3.0f, 0.1f, 5.6438562f}, // Classic: 0.1 .. 5 Hz
    {6.0f, 1.0f, 0.1f, 3.9068906f},  // Dimension: 0.1 .. 1.5 Hz
    {10.0f, 3.0f, 0.2f, 3.9068906f}, // Ensemble: 0.2 .. 3 Hz, plus 6.5 Hz
};
constexpr std::size_t kChorusMask = Chorus::kMemFloats - 1;
} // namespace

float Chorus::RateHz(uint8_t model, int16_t knob) {
    const ChorusShape &s = kChorusShapes[model < 3 ? model : 0];
    return s.rate_lo * Exp2(Knob(knob) * s.log2_rate_range);
}
float Chorus::DepthMs(uint8_t model, int16_t knob) { return kChorusShapes[model < 3 ? model : 0].max_depth_ms * Knob(knob); }

void Chorus::Init(float *mem) {
    buf_ = mem;
    std::memset(buf_, 0, kMemFloats * sizeof(float));
    w_ = 0;
}

void Chorus::Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]) {
    on_ = on;
    model_ = model <= kEnsemble ? model : kClassic;
    base_ = kChorusShapes[model_].base_ms * (kSampleRate / 1000.0f);
    depth_ = DepthMs(model_, k[1]) * (kSampleRate / 1000.0f);
    rate_ = RateHz(model_, k[0]) / kSampleRate;
    // Equal power: dry cos, wet sin of MIX * 90 degrees. Several voices are scaled between summing
    // as uncorrelated (the treble) and as identical (the bass, where their small delay differences
    // barely matter): Dimension's two by 1/sqrt(2), Ensemble's three by 1/2.
    const float m = Knob(k[2]);
    dry_ = Sin2Pi(0.25f * m + 0.25f);
    wet_ = Sin2Pi(0.25f * m) * (model_ == kEnsemble ? 0.5f : model_ == kDimension ? 0.70710678f : 1.0f);
    if (m <= 0.0f)
        dry_ = 1.0f, wet_ = 0.0f; // exactly dry
    if (fade_ <= 0.0f) {          // switching on from off: start at the settings, no ramp
        dry_now_ = dry_;
        wet_now_ = wet_;
        depth_now_ = depth_;
    }
}

// 4-point Hermite interpolation `delay` samples back from the last sample written.
float Chorus::Read(float delay) const {
    const float rp = static_cast<float>(w_) - delay;
    const float fl = Floor(rp);
    const float f = rp - fl;
    const std::size_t i = static_cast<std::size_t>(static_cast<int>(fl) + static_cast<int>(kMemFloats));
    const float y0 = buf_[(i - 1) & kChorusMask], y1 = buf_[i & kChorusMask], y2 = buf_[(i + 1) & kChorusMask],
                y3 = buf_[(i + 2) & kChorusMask];
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void Chorus::Process(float *x, std::size_t n) {
    bool on;
    uint8_t model;
    int16_t k[kMaxKnobs];
    if (settings_.Take(on, model, k))
        Apply(on, model, k);
    if (!on_ && fade_ <= 0)
        return;
    const float dry_step = (dry_ - dry_now_) / n, wet_step = (wet_ - wet_now_) / n, depth_step = (depth_ - depth_now_) / n;
    for (std::size_t i = 0; i < n; ++i) {
        buf_[w_] = x[i];
        dry_now_ += dry_step;
        wet_now_ += wet_step;
        depth_now_ += depth_step;
        phase_ += rate_;
        if (phase_ >= 1.0f)
            phase_ -= 1.0f;
        float wet;
        if (model_ == kDimension) {
            const float tri = 4.0f * Abs(phase_ - 0.5f) - 1.0f; // -1 .. 1
            wet = Read(base_ + depth_now_ * tri) + Read(base_ - depth_now_ * tri);
        } else if (model_ == kEnsemble) {
            phase2_ += 6.5f / kSampleRate;
            if (phase2_ >= 1.0f)
                phase2_ -= 1.0f;
            wet = 0;
            for (int v = 0; v < 3; ++v) {
                float p = phase_ + v * (1.0f / 3.0f), q = phase2_ + v * (1.0f / 3.0f);
                p -= p >= 1.0f ? 1.0f : 0.0f;
                q -= q >= 1.0f ? 1.0f : 0.0f;
                wet += Read(base_ + depth_now_ * (Sin2Pi(p) + 0.05f * Sin2Pi(q)));
            }
        } else {
            wet = Read(base_ + depth_now_ * Sin2Pi(phase_));
        }
        w_ = (w_ + 1) & kChorusMask;
        FadeTowards(fade_, on_);
        x[i] += fade_ * (dry_now_ * x[i] + wet_now_ * wet - x[i]);
    }
    dry_now_ = dry_;
    wet_now_ = wet_;
    depth_now_ = depth_;
}

// Rig ----------------------------------------------------------------------------------------------

namespace {
// The touchscreen's default chain: GATE COMP DRIVE AMP CHORUS DELAY REVERB.
constexpr uint8_t kDefaultOrder[] = {Rig::kGate, Rig::kComp, Rig::kDrive, Rig::kAmp, Rig::kChorus, Rig::kDelay, Rig::kReverb, 0};
} // namespace

int Rig::Slot(uint8_t fx) {
    switch (fx) {
    case kGate: return 0;
    case kComp: return 1;
    case kDrive: return 2;
    case kChorus: return 3;
    case kDelay: return 4;
    case kReverb: return 5;
    default: return -1;
    }
}
bool Rig::Runs(uint8_t fx) { return Slot(fx) >= 0; }

void Rig::Init(float *delay_mem, float *reverb_mem, float *chorus_mem) {
    delay_.Init(delay_mem);
    reverb_.Init(reverb_mem);
    chorus_.Init(chorus_mem);
}

void Rig::Push(uint8_t fx) {
    const State &s = state_[Slot(fx)];
    switch (fx) {
    case kGate: gate_.Set(s.on, s.model, s.knobs); break;
    case kComp: comp_.Set(s.on, s.model, s.knobs); break;
    case kDrive: drive_.Set(s.on, s.model, s.knobs); break;
    case kChorus: chorus_.Set(s.on, s.model, s.knobs); break;
    case kDelay: delay_.Set(s.on, s.model, s.knobs); break;
    case kReverb: reverb_.Set(s.on, s.model, s.knobs); break;
    default: break;
    }
}

void Rig::SetFx(uint8_t fx, bool on, uint8_t model) {
    if (Slot(fx) < 0)
        return;
    state_[Slot(fx)].on = on;
    state_[Slot(fx)].model = model;
    Push(fx);
}

void Rig::SetParam(uint16_t id, int16_t value) {
    const uint8_t fx = static_cast<uint8_t>(id >> 8), knob = static_cast<uint8_t>(id & 0xFF);
    if (Slot(fx) < 0 || knob >= kMaxKnobs)
        return;
    state_[Slot(fx)].knobs[knob] = value;
    Push(fx);
}

void Rig::SetChain(const uint8_t *fx, uint8_t len) {
    const uint32_t gen = chain_gen_.load(std::memory_order_relaxed) + 1;
    uint8_t *slot = chain_[gen & 1];
    for (int i = 0; i < kChainMax; ++i)
        slot[i] = i < len ? fx[i] : 0;
    chain_gen_.store(gen, std::memory_order_release);
}

// Audio thread: pick up a new chain order, keeping the last one if SetChain ran during the copy.
void Rig::LoadOrder() {
    const uint32_t gen = chain_gen_.load(std::memory_order_acquire);
    if (gen == chain_seen_)
        return;
    uint8_t copy[kChainMax];
    for (int i = 0; i < kChainMax; ++i)
        copy[i] = chain_[gen & 1][i];
    if (chain_gen_.load(std::memory_order_acquire) != gen)
        return; // try again next block
    for (int i = 0; i < kChainMax; ++i)
        order_[i] = copy[i];
    chain_seen_ = gen;
}

bool Rig::BeforeAmp(uint8_t fx) const {
    const uint32_t gen = chain_gen_.load(std::memory_order_acquire);
    const uint8_t *o = gen ? chain_[gen & 1] : kDefaultOrder;
    for (int i = 0; i < kChainMax; ++i) {
        if (o[i] == fx)
            return true;
        if (o[i] == kAmp || !o[i])
            return false;
    }
    return false;
}

void Rig::Run(float *x, std::size_t n, bool pre) {
    if (pre) // once per block, before the amp
        LoadOrder();
    const uint8_t *o = chain_seen_ ? order_ : kDefaultOrder;
    bool before_amp = true;
    for (int i = 0; i < kChainMax; ++i) {
        const uint8_t fx = o[i];
        if (!fx)
            break;
        if (fx == kAmp) {
            before_amp = false;
            continue;
        }
        if (before_amp != pre)
            continue;
        switch (fx) {
        case kGate: gate_.Process(x, n); break;
        case kComp: comp_.Process(x, n); break;
        case kDrive: drive_.Process(x, n); break;
        case kChorus: chorus_.Process(x, n); break;
        case kDelay: delay_.Process(x, n); break;
        case kReverb: reverb_.Process(x, n); break;
        default: break;
        }
    }
}

} // namespace fx
