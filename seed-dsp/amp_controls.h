#pragma once
// The amp's own controls around a NAM capture: each amp's real knobs, modelled on its circuit.
//
// A NAM capture is the amp at one fixed setting, so the knobs here move the sound relative to the
// capture: every knob at 50 (Fender Bright off) is exactly the capture. The five knob slots match
// the touchscreen's AMP knobs (wire ids amp.gain .. amp.level); what each slot is depends on the amp:
//
//              slot 0      1      2         3        4
//   Fender     VOLUME      BASS   MIDDLE    TREBLE   BRIGHT (switch: on at 50 and up)
//   Vox        VOLUME      BASS   CUT       TREBLE   MASTER
//   Marshall   PREAMP      BASS   MIDDLE    TREBLE   PRESENCE
//   Generic    GAIN        BASS   MIDDLE    TREBLE   LEVEL     (SD profiles: amp unknown)
//
// Volume/Preamp/Gain drive the capture harder or softer (before it). Bass/Middle/Treble run each
// amp's tone-stack circuit (the Yeh & Smith analytic model of the Fender/Marshall/Vox stack with
// that amp's component values) and divide out the capture's own stack at noon with a fixed fitted
// correction (within about 2 dB of the circuit at any setting). Fender Bright is the bright cap on
// the volume pot (strongest at low volume), Vox Cut the power amp's top cut, Marshall Presence the
// power amp's high-frequency feedback, Master and Level an output gain.
//
// Threading: Configure() from the control thread (it allocates nothing, but does the maths);
// ProcessPre/ProcessPost from the audio thread. A new configuration is picked up at the start of
// the next block; gains glide across that block.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ampctl {

enum class Voicing : uint8_t { Fender, Vox, Marshall, Generic };
constexpr int kSlots = 5;

// Label of a knob slot for an amp, e.g. "CUT"; whether that slot is an on/off switch.
const char *SlotLabel(Voicing v, int slot);
bool SlotIsSwitch(Voicing v, int slot);
// The voicing of a built-in AmpId (1 Fender, 2 Vox, 3 Marshall); anything else is Generic.
Voicing VoicingForAmpId(int amp_id);

class AmpControls {
  public:
    explicit AmpControls(double sample_rate = 48000.0);

    // Control thread: set the amp and its five knobs (0..100, UI units).
    void Configure(Voicing v, const int16_t knobs[kSlots]);

    // Audio thread, in place. Pre: before the capture (drive, bright). Post: after it (tone
    // stack, cut / presence, master / level).
    void ProcessPre(float *x, std::size_t n);
    void ProcessPost(float *x, std::size_t n);

    // Control thread: the post chain's response relative to the capture, in dB, for the last
    // Configure(); and the drive in dB. For logs and tests.
    double PostResponseDb(double hz) const;
    double DriveDb() const;

    // Filter clears, e.g. after the audio was stopped.
    void Reset();

  private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double Response(double hz, double fs) const;
    };
    struct Coeffs {
        // Pre: drive gain and the bright shelf.
        float drive = 1.0f;
        Biquad bright;
        // Post: the tone stack (3rd order), its fixed correction (5 bands), the extra filter
        // (Vox cut, Marshall presence) and the output gain.
        double ts_b[4] = {1, 0, 0, 0}, ts_a[4] = {1, 0, 0, 0};
        double ts_gain = 1.0;
        Biquad corr[5];
        Biquad extra;
        float level = 1.0f;
    };
    void Build(Voicing v, const int16_t knobs[kSlots], Coeffs &c) const;
    void FitCorrection(Voicing v, Biquad corr[5], double &gain) const;
    double StackDb(Voicing v, double t, double m, double l, double hz) const;
    void PickUp();

    double fs_;
    // Triple buffer: Configure() fills buffers_[write_] and swaps it into middle_ flagged kNew;
    // the audio thread swaps middle_ into read_ when flagged.
    static constexpr int kNew = 4;
    Coeffs buffers_[3];
    int write_ = 0, read_ = 2;
    std::atomic<int> middle_{1};

    // Control-thread state: the last configuration and the correction for its amp.
    Voicing voicing_ = Voicing::Generic;
    int16_t knobs_[kSlots] = {50, 50, 50, 50, 0};
    bool have_correction_ = false;
    Voicing correction_for_ = Voicing::Generic;
    Biquad correction_[5];
    double correction_gain_ = 1.0;

    // Audio-thread state.
    float drive_now_ = 1.0f, level_now_ = 1.0f;
    double bright_z_[2] = {}, ts_z_[3] = {}, corr_z_[5][2] = {}, extra_z_[2] = {};
};

} // namespace ampctl
