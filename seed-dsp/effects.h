#pragma once
// Time effects for the Seed: delay (Digital, Tape) and reverb (Hall, Room, Plate, Spring), driven by
// the touchscreen's DELAY and REVERB knobs (wire ids delay.* and reverb.*, 0..100 each).
//
// Built for the Seed's constraints: no allocation, no libDaisy, and no libm (the few exp/sin values
// come from small approximations in effects.cpp), so the code stays a few KB of flash. The caller
// owns the delay memory, so on the Seed it can sit in the 64 MB SDRAM (DSY_SDRAM_BSS) instead of
// the internal RAM the amp model needs: Delay::kMemFloats and Reverb::kMemFloats floats.
//
// Both process a mono block in place (dry + wet), after the amp and its cabinet. Turning one off
// stops new audio going in but lets the echoes or the tail ring out ("trails"); once silent, it
// costs nothing.
//
// Threading: Set() from the control thread (it only stores the settings); Process() from the
// audio thread, which picks up new settings at the start of the next block and glides to them.
//
// Every member starts at zero (Init() sets the rest), so a static Delay or Reverb lands in .bss:
// no copy of it is stored in the Seed's flash.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fx {

constexpr float kSampleRate = 48000.0f;

// Common to both: on/off, model index (as the touchscreen's model list) and four knobs.
class Settings {
  public:
    void Set(bool on, uint8_t model, const int16_t knobs[4]);
    // Audio thread: true (and the values) when Set() was called since the last call.
    bool Take(bool &on, uint8_t &model, int16_t knobs[4]);

  private:
    std::atomic<uint32_t> gen_{0};
    uint32_t seen_ = 0;
    std::atomic<bool> on_{false};
    std::atomic<uint8_t> model_{0};
    std::atomic<int16_t> knobs_[4] = {};
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
    void Set(bool on, uint8_t model, const int16_t knobs[4]) { settings_.Set(on, model, knobs); }
    void Process(float *x, std::size_t n);
    bool Active() const { return on_ || ringing_; }

    // For tests and logs: the delay time and feedback for a TIME and FEEDBACK knob.
    static float TimeMs(int16_t knob);
    static float Feedback(int16_t knob);

  private:
    void Apply(bool on, uint8_t model, const int16_t k[4]);

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
    void Set(bool on, uint8_t model, const int16_t knobs[4]) { settings_.Set(on, model, knobs); }
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
    void Apply(bool on, uint8_t model, const int16_t k[4]);
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

} // namespace fx
