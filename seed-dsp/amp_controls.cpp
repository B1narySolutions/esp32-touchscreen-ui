#include "amp_controls.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace ampctl {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Tone-stack components (Yeh & Smith's FMV stack): R1 slope, R2 bass pot, R3 mid pot (or fixed
// resistor), R4 treble pot, C1 treble cap, C2 bass cap, C3 mid cap.
struct Stack {
    double r1, r2, r3, r4, c1, c2, c3;
    bool fixed_mid; // the Vox Top Boost has no middle pot: R3 is a fixed resistor
};
constexpr Stack kFender{100e3, 1e6, 10e3, 250e3, 250e-12, 0.1e-6, 0.047e-6, false};   // AB763 Twin Reverb
constexpr Stack kMarshall{33e3, 1e6, 25e3, 220e3, 470e-12, 22e-9, 22e-9, false};      // JCM800 2203
constexpr Stack kVox{100e3, 1e6, 10e3, 1e6, 50e-12, 22e-9, 22e-9, true};              // AC30 Top Boost

const Stack &StackFor(Voicing v) {
    switch (v) {
    case Voicing::Fender: return kFender;
    case Voicing::Vox: return kVox;
    default: return kMarshall;
    }
}

// Analog H(s) = (b1 s + b2 s^2 + b3 s^3) / (a0 + a1 s + a2 s^2 + a3 s^3) for pot positions t
// (treble), m (middle) and l (bass), each 0..1; the bass pot is audio taper.
void Analog(const Stack &k, double t, double m, double l, double b[4], double a[4]) {
    const double R1 = k.r1, R2 = k.r2, R3 = k.r3, R4 = k.r4, C1 = k.c1, C2 = k.c2, C3 = k.c3;
    l = std::exp((l - 1.0) * 3.4);
    b[0] = 0;
    b[1] = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    b[2] = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
           (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    b[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + t * C1 * C2 * C3 * R1 * R3 * R4 -
           t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
    a[0] = 1;
    a[1] = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    a[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
           (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    a[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
           l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
}

// Bilinear transform of a 3rd-order analog filter to digital coefficients (normalised, a[0] = 1).
void Bilinear3(const double bs[4], const double as[4], double fs, double bz[4], double az[4]) {
    const double c = 2 * fs, c2 = c * c, c3 = c2 * c;
    bz[0] = bs[0] + bs[1] * c + bs[2] * c2 + bs[3] * c3;
    bz[1] = 3 * bs[0] + bs[1] * c - bs[2] * c2 - 3 * bs[3] * c3;
    bz[2] = 3 * bs[0] - bs[1] * c - bs[2] * c2 + 3 * bs[3] * c3;
    bz[3] = bs[0] - bs[1] * c + bs[2] * c2 - bs[3] * c3;
    az[0] = as[0] + as[1] * c + as[2] * c2 + as[3] * c3;
    az[1] = 3 * as[0] + as[1] * c - as[2] * c2 - 3 * as[3] * c3;
    az[2] = 3 * as[0] - as[1] * c - as[2] * c2 + 3 * as[3] * c3;
    az[3] = as[0] - as[1] * c + as[2] * c2 - as[3] * c3;
    const double n = az[0];
    for (int i = 0; i < 4; ++i) {
        bz[i] /= n;
        az[i] /= n;
    }
}

double Db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

double Response3(const double b[4], const double a[4], double hz, double fs) {
    const std::complex<double> z = std::polar(1.0, -2 * kPi * hz / fs);
    const std::complex<double> num = b[0] + z * (b[1] + z * (b[2] + z * b[3]));
    const std::complex<double> den = a[0] + z * (a[1] + z * (a[2] + z * a[3]));
    return std::abs(num / den);
}

enum class Band { Low, Peak, High };

// RBJ cookbook shelves and peak.
template <class B> void SetBand(B &q, Band kind, double f0, double q_or_s, double gain_db, double fs) {
    const double A = std::pow(10.0, gain_db / 40.0), w = 2 * kPi * f0 / fs, cw = std::cos(w), sw = std::sin(w);
    const double al = sw / (2 * q_or_s);
    double b0, b1, b2, a0, a1, a2;
    if (kind == Band::Peak) {
        b0 = 1 + al * A, b1 = -2 * cw, b2 = 1 - al * A, a0 = 1 + al / A, a1 = -2 * cw, a2 = 1 - al / A;
    } else {
        const double sa = 2 * std::sqrt(A) * al;
        if (kind == Band::Low) {
            b0 = A * ((A + 1) - (A - 1) * cw + sa), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sa;
        }
    }
    q.b0 = b0 / a0, q.b1 = b1 / a0, q.b2 = b2 / a0, q.a1 = a1 / a0, q.a2 = a2 / a0;
}

// The correction's five bands: a low shelf, three peaks, a high shelf.
struct BandSpec {
    Band kind;
    double f0, q;
};
constexpr BandSpec kCorrection[5] = {
    {Band::Low, 90, 0.707}, {Band::Peak, 300, 0.7}, {Band::Peak, 800, 0.7}, {Band::Peak, 2200, 0.7}, {Band::High, 5500, 0.707}};
constexpr int kFitPoints = 40; // 40 Hz .. 15 kHz, log spaced

double FitHz(int i) { return 40.0 * std::pow(15000.0 / 40.0, i / double(kFitPoints - 1)); }

float GainFromDb(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Knob slot to 0..1, and its offset from the capture (noon) as -1..+1.
double Pos(const int16_t k[kSlots], int slot) { return std::clamp(k[slot], int16_t(0), int16_t(100)) / 100.0; }
double FromNoon(const int16_t k[kSlots], int slot) { return (Pos(k, slot) - 0.5) * 2.0; }

// Vox Cut: the power amp's top cut, a first-order low-pass whose corner sweeps 20 kHz -> 1 kHz.
double CutHz(double c) { return 20000.0 * std::pow(1000.0 / 20000.0, c); }

} // namespace

const char *SlotLabel(Voicing v, int slot) {
    static const char *labels[4][kSlots] = {
        {"VOLUME", "BASS", "MIDDLE", "TREBLE", "BRIGHT"},
        {"VOLUME", "BASS", "CUT", "TREBLE", "MASTER"},
        {"PREAMP", "BASS", "MIDDLE", "TREBLE", "PRESENCE"},
        {"GAIN", "BASS", "MIDDLE", "TREBLE", "LEVEL"},
    };
    return slot >= 0 && slot < kSlots ? labels[static_cast<int>(v)][slot] : "?";
}

bool SlotIsSwitch(Voicing v, int slot) { return v == Voicing::Fender && slot == 4; }

Voicing VoicingForAmpId(int amp_id) {
    switch (amp_id) {
    case 1: return Voicing::Fender;
    case 2: return Voicing::Vox;
    case 3: return Voicing::Marshall;
    default: return Voicing::Generic;
    }
}

double AmpControls::Biquad::Response(double hz, double fs) const {
    const std::complex<double> z = std::polar(1.0, -2 * kPi * hz / fs);
    return std::abs((b0 + z * (b1 + z * b2)) / (1.0 + z * (a1 + z * a2)));
}

AmpControls::AmpControls(double sample_rate) : fs_(sample_rate) {
    const int16_t noon[kSlots] = {50, 50, 50, 50, 0};
    Configure(Voicing::Generic, noon);
    const Coeffs first = buffers_[middle_.load() & 3];
    for (auto &b : buffers_)
        b = first;
}

double AmpControls::StackDb(Voicing v, double t, double m, double l, double hz) const {
    const Stack &k = StackFor(v);
    double bs[4], as[4], bz[4], az[4];
    Analog(k, t, k.fixed_mid ? 1.0 : m, l, bs, as);
    Bilinear3(bs, as, fs_, bz, az);
    return Db(Response3(bz, az, hz, fs_));
}

// Fits the five correction bands to the inverse of the capture's stack (all pots at noon), so the
// stack at noon times the correction is flat. Least squares in dB, three passes.
void AmpControls::FitCorrection(Voicing v, Biquad corr[5], double &gain) const {
    double ref[kFitPoints], mean = 0;
    for (int i = 0; i < kFitPoints; ++i) {
        ref[i] = StackDb(v, 0.5, 0.5, 0.5, FitHz(i));
        mean += ref[i] / kFitPoints;
    }
    gain = std::pow(10.0, -mean / 20.0);
    double g[5] = {};
    for (int pass = 0; pass < 3; ++pass) {
        // Residual against what the bands do now, and each band's response to +1 dB.
        double r[kFitPoints], basis[kFitPoints][5];
        for (int b = 0; b < 5; ++b)
            SetBand(corr[b], kCorrection[b].kind, kCorrection[b].f0, kCorrection[b].q, g[b], fs_);
        for (int i = 0; i < kFitPoints; ++i) {
            double now = 0;
            for (int b = 0; b < 5; ++b)
                now += Db(corr[b].Response(FitHz(i), fs_));
            r[i] = -(ref[i] - mean) - now;
            for (int b = 0; b < 5; ++b) {
                Biquad unit;
                SetBand(unit, kCorrection[b].kind, kCorrection[b].f0, kCorrection[b].q, 1.0, fs_);
                basis[i][b] = Db(unit.Response(FitHz(i), fs_));
            }
        }
        // Normal equations, solved by Gauss-Jordan elimination.
        double m[5][6] = {};
        for (int a = 0; a < 5; ++a) {
            for (int b = 0; b < 5; ++b)
                for (int i = 0; i < kFitPoints; ++i)
                    m[a][b] += basis[i][a] * basis[i][b];
            for (int i = 0; i < kFitPoints; ++i)
                m[a][5] += basis[i][a] * r[i];
        }
        for (int c = 0; c < 5; ++c) {
            const double p = m[c][c];
            for (int j = 0; j < 6; ++j)
                m[c][j] /= p;
            for (int rr = 0; rr < 5; ++rr) {
                if (rr == c)
                    continue;
                const double f = m[rr][c];
                for (int j = 0; j < 6; ++j)
                    m[rr][j] -= f * m[c][j];
            }
        }
        for (int b = 0; b < 5; ++b)
            g[b] = std::clamp(g[b] + m[b][5], -24.0, 24.0);
    }
    for (int b = 0; b < 5; ++b)
        SetBand(corr[b], kCorrection[b].kind, kCorrection[b].f0, kCorrection[b].q, g[b], fs_);
}

void AmpControls::Build(Voicing v, const int16_t k[kSlots], Coeffs &c) const {
    // Drive: -20 dB at 0, the capture at 50, +14 dB at 100.
    const double d = FromNoon(k, 0);
    c.drive = GainFromDb(d < 0 ? d * 20.0 : d * 14.0);

    // Fender Bright: the bright cap's treble lift, strongest at low volume, gone at full volume.
    const double bright_db = (v == Voicing::Fender && Pos(k, 4) >= 0.5) ? 9.0 * (1.0 - Pos(k, 0)) : 0.0;
    SetBand(c.bright, Band::High, 1200.0, 0.707, bright_db, fs_);

    // The tone stack at these knobs, and the fixed correction for the capture's own stack.
    const Stack &s = StackFor(v);
    double bs[4], as[4];
    Analog(s, Pos(k, 3), s.fixed_mid ? 1.0 : Pos(k, 2), Pos(k, 1), bs, as);
    Bilinear3(bs, as, fs_, c.ts_b, c.ts_a);
    for (int b = 0; b < 5; ++b)
        c.corr[b] = correction_[b];
    c.ts_gain = correction_gain_;

    // The extra filter and output level of each amp.
    c.extra = Biquad{};
    double level_db = 0;
    switch (v) {
    case Voicing::Vox: {
        // Cut relative to the capture's (noon): (1 + s/w_noon) / (1 + s/w_now), first order.
        const double w0 = 2 * kPi * CutHz(0.5), wk = 2 * kPi * CutHz(Pos(k, 2)), cc = 2 * fs_;
        const double n0 = 1 + cc / w0, n1 = 1 - cc / w0, d0 = 1 + cc / wk, d1 = 1 - cc / wk;
        c.extra.b0 = n0 / d0, c.extra.b1 = n1 / d0, c.extra.a1 = d1 / d0;
        level_db = FromNoon(k, 4) * 12.0; // Master
        break;
    }
    case Voicing::Marshall:
        SetBand(c.extra, Band::High, 3500.0, 0.707, FromNoon(k, 4) * 6.0, fs_); // Presence
        break;
    case Voicing::Generic:
        level_db = FromNoon(k, 4) * 12.0; // Level
        break;
    case Voicing::Fender:
        break;
    }
    c.level = GainFromDb(level_db);
}

void AmpControls::Configure(Voicing v, const int16_t knobs[kSlots]) {
    if (!have_correction_ || correction_for_ != v) {
        FitCorrection(v, correction_, correction_gain_);
        correction_for_ = v;
        have_correction_ = true;
    }
    voicing_ = v;
    for (int i = 0; i < kSlots; ++i)
        knobs_[i] = knobs[i];
    // Fill the spare buffer and swap it into the middle, flagged new (triple buffering: the audio
    // thread never waits and never sees a half-written set).
    Build(v, knobs, buffers_[write_]);
    write_ = middle_.exchange(write_ | kNew, std::memory_order_acq_rel) & 3;
}

void AmpControls::PickUp() {
    if (middle_.load(std::memory_order_acquire) & kNew)
        read_ = middle_.exchange(read_, std::memory_order_acq_rel) & 3;
}

void AmpControls::Reset() {
    std::fill(std::begin(bright_z_), std::end(bright_z_), 0.0);
    std::fill(std::begin(ts_z_), std::end(ts_z_), 0.0);
    for (auto &z : corr_z_)
        z[0] = z[1] = 0;
    extra_z_[0] = extra_z_[1] = 0;
}

namespace {
inline double RunBiquad(double x, double b0, double b1, double b2, double a1, double a2, double z[2]) {
    const double y = b0 * x + z[0];
    z[0] = b1 * x - a1 * y + z[1];
    z[1] = b2 * x - a2 * y;
    return y;
}
} // namespace

void AmpControls::ProcessPre(float *x, std::size_t n) {
    PickUp();
    const Coeffs &c = buffers_[read_];
    const float step = (c.drive - drive_now_) / static_cast<float>(n);
    const Biquad &q = c.bright;
    for (std::size_t i = 0; i < n; ++i) {
        drive_now_ += step;
        const double y = RunBiquad(x[i] * drive_now_, q.b0, q.b1, q.b2, q.a1, q.a2, bright_z_);
        x[i] = static_cast<float>(y);
    }
    drive_now_ = c.drive;
}

void AmpControls::ProcessPost(float *x, std::size_t n) {
    const Coeffs &c = buffers_[read_];
    const float step = (c.level - level_now_) / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Tone stack, 3rd order, transposed direct form II.
        const double in = x[i];
        double y = c.ts_b[0] * in + ts_z_[0];
        ts_z_[0] = c.ts_b[1] * in - c.ts_a[1] * y + ts_z_[1];
        ts_z_[1] = c.ts_b[2] * in - c.ts_a[2] * y + ts_z_[2];
        ts_z_[2] = c.ts_b[3] * in - c.ts_a[3] * y;
        y *= c.ts_gain;
        for (int b = 0; b < 5; ++b) {
            const Biquad &q = c.corr[b];
            y = RunBiquad(y, q.b0, q.b1, q.b2, q.a1, q.a2, corr_z_[b]);
        }
        y = RunBiquad(y, c.extra.b0, c.extra.b1, c.extra.b2, c.extra.a1, c.extra.a2, extra_z_);
        level_now_ += step;
        x[i] = static_cast<float>(y) * level_now_;
    }
    level_now_ = c.level;
}

double AmpControls::PostResponseDb(double hz) const {
    Coeffs c;
    Build(voicing_, knobs_, c);
    double db = Db(Response3(c.ts_b, c.ts_a, hz, fs_)) + Db(c.ts_gain) + Db(c.extra.Response(hz, fs_)) + Db(c.level);
    for (const auto &q : c.corr)
        db += Db(q.Response(hz, fs_));
    return db;
}

double AmpControls::DriveDb() const {
    Coeffs c;
    Build(voicing_, knobs_, c);
    return Db(c.drive);
}

} // namespace ampctl
