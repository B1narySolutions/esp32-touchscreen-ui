#pragma once
// The touchscreen's effects, for the Seed: GATE, COMP and DRIVE (pedals before the amp), CHORUS,
// DELAY and REVERB (after it), driven by its knobs (wire ids gate.*, comp.*, ..., 0..100 each).
// Rig runs them all in the touchscreen's chain order; see docs/EFFECTS.md.
//
// Built for the Seed's constraints: no allocation, no libDaisy, and no libm (the few exp/sin values
// come from small approximations in effects.cpp), so the code stays a few KB of flash. The caller
// owns the delay memory, so on the Seed it can sit in the 64 MB SDRAM (DSY_SDRAM_BSS) instead of
// the internal RAM the amp model needs: Delay::kMemFloats and Reverb::kMemFloats floats.
//
// Each processes a mono block in place. Turning delay or reverb off stops new audio going in but
// lets the echoes or the tail ring out ("trails"); the others crossfade to the dry signal in 5 ms.
// Once off and silent, an effect costs nothing.
//
// Threading: Set() from the control thread (it only stores the settings); Process() from the
// audio thread, which picks up new settings at the start of the next block and glides to them.
//
// Every member starts at zero (Init() sets the rest), so a static effect lands in .bss: no copy of
// it is stored in the Seed's flash.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fx {

constexpr float kSampleRate = 48000.0f;

constexpr int kMaxKnobs = 5;

// Common to all: on/off, model index (as the touchscreen's model list) and up to five knobs.
class Settings {
  public:
    void Set(bool on, uint8_t model, const int16_t *knobs, int count);
    // Audio thread: true (and the values) when Set() was called since the last call.
    bool Take(bool &on, uint8_t &model, int16_t knobs[kMaxKnobs]);

  private:
    std::atomic<uint32_t> gen_{0};
    uint32_t seen_ = 0;
    std::atomic<bool> on_{false};
    std::atomic<uint8_t> model_{0};
    std::atomic<int16_t> knobs_[kMaxKnobs] = {};
};

// Knobs: THRESHOLD (-80 .. -20 dBFS: below it the gate closes, 6 dB of hysteresis and a 10 ms hold
// keep it from chattering), ATTACK (0.1 .. 20 ms to open), RELEASE (10 ms .. 1 s to close), RANGE
// (how far it turns down when closed: 0 .. 90 dB).
class Gate {
  public:
    void Set(bool on, uint8_t, const int16_t knobs[4]) { settings_.Set(on, 0, knobs, 4); }
    void Process(float *x, std::size_t n);
    static float ThresholdDb(int16_t knob);
    static float RangeDb(int16_t knob);

  private:
    void Apply(bool on, const int16_t k[kMaxKnobs]);
    Settings settings_;
    bool on_ = false;
    float fade_ = 0;                // 0 = bypassed .. 1 = on
    float open_ = 0, close_ = 0;    // thresholds (linear)
    float att_ = 0, rel_ = 0, floor_ = 0;
    float env_ = 0, gain_ = 0;      // detector level; gain (set to its target on the first sample)
    uint32_t hold_ = 0;
    bool is_open_ = false, started_ = false;
};

// Knobs: THRESHOLD (-60 .. 0 dBFS), RATIO (1:1 .. 20:1), ATTACK (0.1 .. 100 ms), RELEASE (20 ms ..
// 1 s), GAIN (make-up, -12 .. +24 dB). Feed-forward, peak detector, 6 dB soft knee.
class Compressor {
  public:
    void Set(bool on, uint8_t, const int16_t knobs[5]) { settings_.Set(on, 0, knobs, 5); }
    void Process(float *x, std::size_t n);
    static float ThresholdDb(int16_t knob);
    static float Ratio(int16_t knob);
    static float MakeupDb(int16_t knob);
    // The static curve: output level for an input level (dB), for tests and logs.
    static float CurveDb(float in_db, float threshold_db, float ratio);
    float ReductionDb() const { return reduction_db_; } // the last block's gain reduction

  private:
    void Apply(bool on, const int16_t k[kMaxKnobs]);
    Settings settings_;
    bool on_ = false;
    float fade_ = 0;
    float thr_ = 0, ratio_ = 1, att_ = 0, rel_ = 0, makeup_db_ = 0;
    float level_ = 0;               // detected peak level (linear)
    float reduction_db_ = 0;
};

// 2x up- and downsampling with a half-band filter of `Pairs` coefficient pairs (least-squares
// designs: 16 pairs, flat to 18 kHz and -116 dB from 30 kHz at 48 -> 96 kHz; 8 pairs, flat to 24 kHz
// and -126 dB from 72 kHz at 96 -> 192 kHz). The drive's gain of up to 66 dB lifts whatever the
// filters leave, so they need to reject that deep. Delay: 2 * Pairs - 1 input samples.
template <int Pairs> class HalfBand2x {
  public:
    static constexpr int kLatency = 2 * Pairs - 1;
    // One input sample in, the two output-rate samples out (in time order).
    void Up(float x, float &first, float &second);
    // Two samples in (in time order), one out.
    float Down(float first, float second);

  private:
    static const float kCoefs[Pairs];
    float up_[4 * Pairs] = {}; // histories, stored twice (mirrored) so reads never wrap
    float down_odd_[4 * Pairs] = {}, down_even_[4 * Pairs] = {};
    int up_pos_ = 0, down_pos_ = 0;
};

// Knobs: GAIN, TONE (dark .. bright), LEVEL (-30 .. +10 dB). Models, each the classic pedal's
// structure: 0 Tube Screamer (720 Hz high-pass into a soft clipper, clean signal added back: the
// mid hump), 1 Big Muff (two cascaded clipping stages; tone blends a low-pass and a high-pass, which
// scoops the mids), 2 Klon-style (soft clipping blended with more clean signal the lower the gain;
// treble tone), 3 Rat (up to 66 dB of gain whose bandwidth shrinks with gain, as the LM308's does,
// into hard diode clipping; filter tone). Against aliasing, the clipping runs at 192 kHz (two
// HalfBand2x stages) through an antiderivative anti-aliased clipper; the pedal adds about 27.5
// samples (0.57 ms) of delay while it's on.
class Drive {
  public:
    enum Model : uint8_t { kScreamer = 0, kMuff = 1, kKlon = 2, kRat = 3 };
    void Set(bool on, uint8_t model, const int16_t knobs[3]) { settings_.Set(on, model, knobs, 3); }
    void Process(float *x, std::size_t n);
    static float GainDb(uint8_t model, int16_t knob);

  private:
    void Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]);
    float Stage(float r); // 96 kHz: the gain stage, clipped at 192 kHz, plus the clean signal
    float Shape(float s); // 192 kHz: gain and clipping
    // Cubic soft clipper (slope 1 at 0, +-1 from |x| = 1.5) with first-order antiderivative
    // anti-aliasing: the mean of the curve between the previous input and this one.
    static float Clip(float x, float &prev);
    Settings settings_;
    HalfBand2x<12> outer_; // 48 <-> 96 kHz
    HalfBand2x<5> inner_;  // 96 <-> 192 kHz
    bool on_ = false;
    uint8_t model_ = 0;
    float fade_ = 0;
    float gain_ = 0, gain2_ = 0, clean_ = 0, clip_ = 0, level_ = 0;
    float in_hp_a_ = 0, in_hp_z_ = 0;        // into the gain stage (96 kHz)
    float bw_a_ = 0, bw_z_ = 0;              // Rat: the op-amp's bandwidth (192 kHz)
    float clip_prev_[2] = {};                // each clipper's previous input (anti-aliasing)
    float mid_hp_a_ = 0, mid_hp_z_ = 0;      // Muff: coupling between the stages (192 kHz)
    float tone_lp_a_ = 0, tone_lp_z_ = 0, tone_hp_a_ = 0, tone_hp_z_ = 0, tone_ = 0;
    float clean_hist_[16] = {};              // the clean signal at 96 kHz, delayed to line up
    int clean_pos_ = 0;
};

// Knobs: RATE, DEPTH, MIX (0 = dry, 50 = both, 100 = only the modulated voices: vibrato; an
// equal-power crossfade, so the level stays about the same). Models: 0 Classic
// (one voice, sine LFO 0.1 .. 5 Hz, up to +-3 ms around 12 ms), 1 Dimension (two voices on opposite
// triangle LFOs, shallow, 0.1 .. 1.5 Hz: depth without an obvious wobble), 2 Ensemble (three voices
// 120 degrees apart, plus a faster 6.5 Hz shimmer, as string ensembles do). Mono output.
class Chorus {
  public:
    static constexpr std::size_t kMemFloats = 2048; // 42 ms
    enum Model : uint8_t { kClassic = 0, kDimension = 1, kEnsemble = 2 };
    void Init(float *mem);
    void Set(bool on, uint8_t model, const int16_t knobs[3]) { settings_.Set(on, model, knobs, 3); }
    void Process(float *x, std::size_t n);
    static float RateHz(uint8_t model, int16_t knob);
    static float DepthMs(uint8_t model, int16_t knob);

  private:
    void Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]);
    float Read(float delay) const;
    Settings settings_;
    float *buf_ = nullptr;
    std::size_t w_ = 0;
    bool on_ = false;
    uint8_t model_ = 0;
    float fade_ = 0;
    float base_ = 0, depth_ = 0, rate_ = 0, depth_now_ = 0;
    float dry_ = 0, wet_ = 0, dry_now_ = 0, wet_now_ = 0;
    float phase_ = 0, phase2_ = 0;
};

// Knobs: TIME (40 ms .. 1 s), FEEDBACK (repeats), TONE (repeats dark .. bright), MIX (echo level,
// up to as loud as the dry signal). Models: 0 Digital (clean repeats), 1 Tape (each repeat a bit
// darker and saturated, with slight wow and flutter).
class Delay {
  public:
    static constexpr std::size_t kMemFloats = 48000 + 1024; // 1 s plus the tape wow
    enum Model : uint8_t { kDigital = 0, kTape = 1 };

    // `mem` must hold kMemFloats floats and outlive the effect.
    void Init(float *mem);
    void Set(bool on, uint8_t model, const int16_t knobs[4]) { settings_.Set(on, model, knobs, 4); }
    void Process(float *x, std::size_t n);
    bool Active() const { return on_ || ringing_; }

    // For tests and logs: the delay time and feedback for a TIME and FEEDBACK knob.
    static float TimeMs(int16_t knob);
    static float Feedback(int16_t knob);

  private:
    void Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]);

    Settings settings_;
    float *buf_ = nullptr;
    std::size_t w_ = 0;
    bool on_ = false, ringing_ = false;
    uint8_t model_ = kDigital;
    float Read(float delay) const;
    // TIME: a change crossfades (30 ms) from the old read position to the new one.
    float delay_target_ = 0, delay_now_ = 0, delay_next_ = 0, xfade_ = 0; // samples (Init() sets them)
    // Targets and their smoothed values.
    float fb_ = 0, wet_ = 0, in_gain_ = 0;
    float fb_now_ = 0, wet_now_ = 0, in_now_ = 0;
    float lp_a_ = 0, lp_z_ = 0;   // repeat tone (one-pole low-pass in the loop)
    float hp_a_ = 0, hp_z_ = 0;   // tape: low cut in the loop
    float wow_phase_ = 0, flutter_phase_ = 0;
    uint32_t quiet_ = 0;          // samples of silence in the loop since it was turned off
};

// Knobs: DECAY (time for the tail to fall 60 dB; range depends on the model), PRE-DELAY (0 .. 100
// ms), TONE (damping: dark .. bright tail), MIX (reverb level, up to about the dry level).
// Models: 0 Hall, 1 Room, 2 Plate (dense, bright) - a comb/allpass network (Schroeder / Freeverb
// topology, sized per model) - and 3 Spring: two dispersive allpass chains in feedback loops,
// which give a spring tank's chirp ("drip").
class Reverb {
  public:
    static constexpr int kCombs = 8, kAllpasses = 4, kSprings = 2, kSpringStages = 24, kSpringStretch = 6;
    static constexpr std::size_t kMemFloats = 25000; // see effects.cpp for the layout
    enum Model : uint8_t { kHall = 0, kRoom = 1, kPlate = 2, kSpring = 3, kNoModel = 0xFF };

    void Init(float *mem);
    void Set(bool on, uint8_t model, const int16_t knobs[4]) { settings_.Set(on, model, knobs, 4); }
    void Process(float *x, std::size_t n);
    bool Active() const { return on_ || ringing_; }

    // For tests and logs: the decay time (RT60, seconds) and pre-delay for the knobs.
    static float DecaySeconds(uint8_t model, int16_t knob);
    static float PredelayMs(int16_t knob);

  private:
    struct Line { // a delay line in the shared memory
        float *buf = nullptr;
        uint32_t len = 0, max = 0, pos = 0;
    };
    void Apply(bool on, uint8_t model, const int16_t k[kMaxKnobs]);
    void Clear();

    Settings settings_;
    float *mem_ = nullptr;
    bool on_ = false, ringing_ = false;
    uint8_t model_ = 0;     // Init() sets kNoModel: the first Apply() lays out the memory
    Line pre_, comb_[kCombs], ap_[kAllpasses], spring_[kSprings];
    float comb_g_[kCombs] = {}, comb_in_[kCombs] = {}, comb_lp_[kCombs] = {};
    float ap_g_ = 0, damp_a_ = 0;
    // Spring: stretched first-order allpass stages (kSpringStretch samples of state each), loop
    // gains and the loop low-pass.
    float sp_x_[kSprings][kSpringStages][kSpringStretch] = {}, sp_y_[kSprings][kSpringStages][kSpringStretch] = {};
    float sp_g_[kSprings] = {}, sp_in_[kSprings] = {}, sp_lp_[kSprings] = {};
    uint32_t sp_phase_ = 0;
    float in_hp_z_ = 0, in_hp_a_ = 0;
    float wet_ = 0, wet_now_ = 0, in_gain_ = 0, in_now_ = 0;
    uint32_t pre_len_ = 0, quiet_ = 0;
};

// All six, as the touchscreen's chain has them: the ones before AMP in the chain run in
// ProcessPre (before the amp model), the rest in ProcessPost (after the amp and cabinet). Takes the
// link's messages as they come: SetFx (SET_FX_STATE), SetParam (SET_PARAMS), SetChain (SET_CHAIN).
// Effects the Seed doesn't run (EQ, octave, phaser, tremolo) are ignored.
class Rig {
  public:
    // The link's effect ids (tools/param_ids.json). A parameter id is (effect << 8) | knob.
    enum : uint8_t { kGate = 1, kComp = 2, kDrive = 4, kAmp = 5, kChorus = 7, kDelay = 10, kReverb = 11 };
    static bool Runs(uint8_t fx);

    // Memory: Delay::kMemFloats, Reverb::kMemFloats and Chorus::kMemFloats floats.
    void Init(float *delay_mem, float *reverb_mem, float *chorus_mem);
    // Control thread.
    void SetFx(uint8_t fx, bool on, uint8_t model);
    void SetParam(uint16_t id, int16_t value);
    void SetChain(const uint8_t *fx, uint8_t len);
    // Audio thread, in place.
    void ProcessPre(float *x, std::size_t n) { Run(x, n, true); }
    void ProcessPost(float *x, std::size_t n) { Run(x, n, false); }

    // Control-thread view, for logs.
    bool On(uint8_t fx) const { return Slot(fx) >= 0 && state_[Slot(fx)].on; }
    uint8_t Model(uint8_t fx) const { return Slot(fx) >= 0 ? state_[Slot(fx)].model : 0; }
    bool BeforeAmp(uint8_t fx) const;

  private:
    struct State {
        bool on;
        uint8_t model;
        int16_t knobs[kMaxKnobs];
    };
    static int Slot(uint8_t fx);
    void Push(uint8_t fx);
    void Run(float *x, std::size_t n, bool pre);
    // The chain order (effect ids, 0 = end; all 0 = the default order), double-buffered: SetChain
    // fills the spare copy and bumps the generation (32-bit: lock-free on the Seed, where 64-bit
    // atomics aren't); the audio thread keeps its last good copy if one changes under it.
    static constexpr int kChainMax = 8;
    void LoadOrder();

    Gate gate_;
    Compressor comp_;
    Drive drive_;
    Chorus chorus_;
    Delay delay_;
    Reverb reverb_;
    State state_[6] = {};
    uint8_t chain_[2][kChainMax] = {};
    std::atomic<uint32_t> chain_gen_{0};
    uint32_t chain_seen_ = 0;          // audio thread
    uint8_t order_[kChainMax] = {};    // audio thread: the order in use
};

} // namespace fx
