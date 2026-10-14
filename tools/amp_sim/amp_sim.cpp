// amp_sim: the Seed's amp engine on the Mac, controlled by the real touchscreen.
//
// Runs Rocco's realtime-nam-seed3 audio code (NamAudio: the A2-Lite amp models, cabinet EQ, volume
// and tone controls) on a simulated guitar, plays it on the Mac's speakers and records it, and
// takes its orders over the link from the touchscreen exactly as the Seed build with the link does
// (slp_receiver.c, the same master/mute ramp and model switching as docs/seed3-link.patch). The amp's
// own knobs run seed-dsp/amp_controls: each amp's real controls around the capture. Every change
// the touchscreen sends is printed with whether it changes the sound, how, and how long it took.
//
// The touchscreen labels everything it shows from here MOCK: the sound is the Seed's code, but the
// DSP load is the Mac's. Build and run with tools/amp_sim/run.sh.
#include "amp_controls.h"
#include "audio/nam_audio.h"
#include "models/amp_models.h"
#include "param_names.h"
#include "slp_params.h"
#include "slp_receiver.h"

#include <AudioToolbox/AudioToolbox.h>
#include <IOKit/serial/ioss.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <random>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = static_cast<int>(NamAudio::kBlockSize);
constexpr double kBlockUs = 1e6 * kBlock / kRate;

std::atomic<bool> g_quit{false};
const auto g_t0 = std::chrono::steady_clock::now();

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count(); }

// ---------------------------------------------------------------------------------------------
// Simulated guitar: Karplus-Strong strings strumming power chords in E, at about DI level.

class Guitar {
  public:
    float Next() {
        // Strum the next chord every 1.6 s, low string first, 12 ms apart.
        if (sample_ % (kRate * 16 / 10) == 0)
            chord_ = (chord_ + 1) % 4;
        const int64_t in_chord = sample_ % (kRate * 16 / 10);
        for (int s = 0; s < 3; ++s)
            if (in_chord == s * kRate * 12 / 1000)
                Pluck(s, kChords[chord_][s]);
        ++sample_;

        float sum = 0.0f;
        for (auto &st : strings_) {
            if (st.buf.empty())
                continue;
            const size_t n = st.buf.size();
            const float out = st.buf[st.pos];
            const float next = st.buf[(st.pos + 1) % n];
            st.buf[st.pos] = (0.7f * out + 0.3f * next) * 0.996f; // brighter than plain averaging
            st.pos = (st.pos + 1) % n;
            sum += out;
        }
        return 0.18f * sum;
    }

  private:
    struct String {
        std::vector<float> buf;
        size_t pos = 0;
    };
    void Pluck(int s, double hz) {
        auto &st = strings_[s];
        st.buf.assign(static_cast<size_t>(kRate / hz), 0.0f);
        std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
        for (auto &v : st.buf)
            v = noise(rng_);
        st.pos = 0;
    }
    static constexpr double kChords[4][3] = {
        {82.41, 123.47, 164.81},  // E5
        {110.00, 164.81, 220.00}, // A5
        {146.83, 220.00, 293.66}, // D5
        {98.00, 146.83, 196.00},  // G5
    };
    String strings_[3];
    int chord_ = 3;
    int64_t sample_ = 0;
    std::mt19937 rng_{1234};
};

// A WAV file instead of the synthesized guitar (48 kHz, 16/24-bit PCM or 32-bit float; first channel).
bool LoadWav(const char *path, std::vector<float> &out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4))
        return false;
    int fmt = 0, channels = 0, bits = 0, rate = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len;
        memcpy(&len, d.data() + p + 4, 4);
        const uint8_t *c = d.data() + p + 8;
        if (!memcmp(d.data() + p, "fmt ", 4)) {
            fmt = c[0] | c[1] << 8;
            channels = c[2] | c[3] << 8;
            memcpy(&rate, c + 4, 4);
            bits = c[14] | c[15] << 8;
        } else if (!memcmp(d.data() + p, "data", 4) && channels) {
            if (rate != kRate)
                fprintf(stderr, "warning: %s is %d Hz, played at 48000 Hz\n", path, rate);
            const int bytes = bits / 8, frame = bytes * channels;
            for (size_t i = 0; i + frame <= len && p + 8 + i + frame <= d.size(); i += frame) {
                const uint8_t *s = c + i;
                float v = 0;
                if (fmt == 3 && bits == 32)
                    memcpy(&v, s, 4);
                else if (bits == 16)
                    v = static_cast<int16_t>(s[0] | s[1] << 8) / 32768.0f;
                else if (bits == 24)
                    v = (static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (s[2] << 24)) >> 8) / 8388608.0f;
                out.push_back(v);
            }
        }
        p += 8 + len + (len & 1);
    }
    return !out.empty();
}

void WriteWav(const std::string &path, const std::vector<int16_t> &s) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;
    const uint32_t data = static_cast<uint32_t>(s.size() * 2), riff = 36 + data, fmt_len = 16, rate = kRate,
                   byte_rate = kRate * 2;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt_len, 4, 1, f);
    fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
    fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// State the link sets (main thread) and the audio engine reads (audio thread), as on the Seed.

std::atomic<float> g_master_target{1.0f};
std::atomic<uint32_t> g_master_gen{0};
std::atomic<int> g_requested_amp{1}; // 0 = bypass, else AmpId
std::atomic<uint32_t> g_amp_gen{0};
std::atomic<bool> g_requested_cab{true};

// The amp's own knobs (seed-dsp/amp_controls): configured here in the main thread, run in the
// audio thread. Slots as the touchscreen's AMP knobs; all at 50 (Fender Bright off) = the capture.
ampctl::AmpControls g_ctl;
int16_t g_amp_knobs[ampctl::kSlots] = {50, 50, 50, 50, 0};
int16_t g_amp_knobs_printed[ampctl::kSlots] = {50, 50, 50, 50, 0};
bool g_amp_knobs_seen = false;  // the touchscreen's first full set (its snapshot) has arrived
int g_amp_knob_updates = 0;
double g_amp_switch_ms = -1e9; // knob values arriving with an amp switch are that amp's, not changes

// What the engine has applied, and when (for the latency lines).
std::atomic<uint32_t> g_master_applied_gen{0}, g_amp_applied_gen{0};
std::atomic<double> g_master_applied_ms{0}, g_amp_applied_ms{0};
std::atomic<int> g_active_amp{1};
std::atomic<bool> g_model_ok{true};

// Measurements for METERS, STATUS and the console.
std::atomic<float> g_in_peak{0}, g_out_peak{0};
std::atomic<uint32_t> g_clips{0}, g_blocks{0}, g_overruns{0};
std::atomic<double> g_proc_us{0}, g_proc_max_us{0}, g_in_sq{0}, g_out_sq{0};
std::atomic<uint64_t> g_level_samples{0};

void RaiseMax(std::atomic<float> &a, float v) {
    float p = a.load(std::memory_order_relaxed);
    while (v > p && !a.compare_exchange_weak(p, v, std::memory_order_relaxed)) {
    }
}
void AddTo(std::atomic<double> &a, double v) {
    double p = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(p, p + v, std::memory_order_relaxed)) {
    }
}

// ---------------------------------------------------------------------------------------------
// The audio engine: one 48-sample block, exactly the Seed's order of operations.

class Engine {
  public:
    explicit Engine(std::vector<float> wav) : wav_(std::move(wav)) {
        audio_.LoadAmpModel(AmpId::Fender);
        audio_.SetKnobs(1.0f, 1.0f); // the carrier's volume and tone knobs, fully up
        audio_.ResetKnobs();
        record_.reserve(static_cast<size_t>(kRate) * 600);
    }

    void Block(float *out) {
        // Model and cabinet changes, as ApplyPendingAmpCommand / ApplyPendingCabinetCommand do.
        const uint32_t amp_gen = g_amp_gen.load(std::memory_order_acquire);
        if (amp_gen != amp_gen_seen_) {
            amp_gen_seen_ = amp_gen;
            const int amp = g_requested_amp.load(std::memory_order_relaxed);
            if (amp != active_amp_) {
                bypass_ = amp == 0;
                if (!bypass_)
                    g_model_ok = audio_.LoadAmpModel(static_cast<AmpId>(amp));
                active_amp_ = amp;
                g_active_amp = amp;
            }
            g_amp_applied_ms = NowMs();
            g_amp_applied_gen = amp_gen;
        }
        const bool cab = g_requested_cab.load(std::memory_order_relaxed);
        if (cab != audio_.CabinetEnabled())
            audio_.SetCabinet(cab);

        // The guitar.
        float in[kBlock], left[kBlock], right[kBlock];
        for (int i = 0; i < kBlock; ++i)
            in[i] = wav_.empty() ? guitar_.Next() : wav_[wav_pos_++ % wav_.size()];

        // The amp's drive, NamAudio, the amp's tone controls, then the touchscreen's master ramped
        // across the block (docs/seed3-link.patch). On the Seed the tone controls belong between
        // the model and the cabinet EQ; both are linear, so after the cabinet sounds the same.
        const auto t0 = std::chrono::steady_clock::now();
        float dry[kBlock];
        std::copy(in, in + kBlock, dry);
        if (!bypass_)
            g_ctl.ProcessPre(in, kBlock);
        audio_.Process(in, left, right, kBlock, bypass_);
        if (!bypass_)
            g_ctl.ProcessPost(left, kBlock);
        const uint32_t master_gen = g_master_gen.load(std::memory_order_acquire);
        const float target = g_master_target.load(std::memory_order_relaxed);
        const float step = (target - gain_) / kBlock;
        float in_max = 0, out_max = 0;
        double in_sq = 0, out_sq = 0;
        for (int i = 0; i < kBlock; ++i) {
            gain_ += step;
            out[i] = std::clamp(left[i] * gain_, -1.0f, 1.0f);
            in[i] = dry[i];
            in_max = std::max(in_max, std::fabs(in[i]));
            out_max = std::max(out_max, std::fabs(out[i]));
            in_sq += in[i] * in[i];
            out_sq += out[i] * out[i];
            if (record_.size() < record_.capacity()) // up to 10 minutes; never reallocate in the audio thread
                record_.push_back(static_cast<int16_t>(std::clamp(out[i], -1.0f, 1.0f) * 32767.0f));
        }
        gain_ = target;
        if (master_gen != master_gen_seen_) {
            master_gen_seen_ = master_gen;
            g_master_applied_ms = NowMs();
            g_master_applied_gen = master_gen;
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();

        // Measurements.
        if (out_max >= 1.0f)
            g_clips.fetch_add(1);
        if (us >= kBlockUs)
            g_overruns.fetch_add(1);
        g_blocks.fetch_add(1);
        AddTo(g_proc_us, us);
        if (us > g_proc_max_us.load())
            g_proc_max_us = us;
        RaiseMax(g_in_peak, in_max);
        RaiseMax(g_out_peak, out_max);
        AddTo(g_in_sq, in_sq);
        AddTo(g_out_sq, out_sq);
        g_level_samples.fetch_add(kBlock);
    }

    const std::vector<int16_t> &Recording() const { return record_; }

  private:
    NamAudio audio_;
    Guitar guitar_;
    std::vector<float> wav_;
    size_t wav_pos_ = 0;
    bool bypass_ = false;
    int active_amp_ = 1;
    float gain_ = 1.0f;
    uint32_t amp_gen_seen_ = 0, master_gen_seen_ = 0;
    std::vector<int16_t> record_;
};

Engine *g_engine;

void AudioQueueCallback(void *, AudioQueueRef q, AudioQueueBufferRef b) {
    auto *out = static_cast<float *>(b->mAudioData);
    const uint32_t frames = b->mAudioDataBytesCapacity / sizeof(float);
    for (uint32_t i = 0; i + kBlock <= frames; i += kBlock)
        g_engine->Block(out + i);
    b->mAudioDataByteSize = frames * sizeof(float);
    AudioQueueEnqueueBuffer(q, b, 0, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Serial port at 1 Mbaud (the touchscreen's "USB TO UART" port, SW1 on UART1).

int g_fd = -1;

bool OpenPort(const char *port) {
    g_fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_fd < 0)
        return false;
    termios tio{};
    if (tcgetattr(g_fd, &tio))
        return false;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS | HUPCL);
    tio.c_cc[VMIN] = tio.c_cc[VTIME] = 0;
    if (tcsetattr(g_fd, TCSANOW, &tio))
        return false;
    speed_t speed = 1000000;
    if (ioctl(g_fd, IOSSIOSPEED, &speed))
        return false;
    int lines = TIOCM_DTR | TIOCM_RTS; // don't reset the ESP32 through its auto-reset circuit
    ioctl(g_fd, TIOCMBIC, &lines);
    tcflush(g_fd, TCIOFLUSH);
    return true;
}

void PortSend(void *, const uint8_t *b, size_t n) {
    while (n) {
        const ssize_t w = write(g_fd, b, n);
        if (w > 0) {
            b += w;
            n -= static_cast<size_t>(w);
        } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
            return;
        } else {
            pollfd p{g_fd, POLLOUT, 0};
            poll(&p, 1, 2);
        }
    }
}

uint32_t LinkNow(void *) { return static_cast<uint32_t>(NowMs()); }

// ---------------------------------------------------------------------------------------------
// Hooks: what the touchscreen sends, printed with whether it changes the sound.

struct PendingLatency {
    double received_ms;
    uint32_t gen;
    bool amp; // else master
    std::string what;
};
std::vector<PendingLatency> g_pending;
std::map<uint16_t, int16_t> g_params;          // last value per parameter
std::map<uint16_t, int16_t> g_params_changed;  // changed since the last print
std::map<int, std::pair<bool, int>> g_fx;      // fx -> (on, model)
uint8_t g_master_volume = 255;
bool g_muted = false;

const char *FxName(uint8_t fx) {
    static const char *names[] = {"?", "GATE", "COMP", "EQ", "DRIVE", "AMP", "OCTAVE", "CHORUS", "PHASER", "TREMOLO", "DELAY", "REVERB", "CAB"};
    return fx < sizeof names / sizeof names[0] ? names[fx] : "?";
}

void Event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("%9.3f  ", NowMs() / 1000.0);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void OnMaster(void *, uint8_t volume, bool muted) {
    if (volume == g_master_volume && muted == g_muted)
        return; // a snapshot repeating what the Seed already has
    const float v = volume / 100.0f, gain = muted ? 0.0f : v * v;
    char what[96];
    if (muted != g_muted)
        snprintf(what, sizeof what, "%s (gain %.2f)", muted ? "MUTE on" : "MUTE off", gain);
    else if (g_master_volume == 255)
        snprintf(what, sizeof what, "MASTER %u (gain %.2f, %+.1f dB)", volume, gain, gain > 0 ? 20 * std::log10(gain) : -INFINITY);
    else
        snprintf(what, sizeof what, "MASTER %u -> %u (gain %.2f, %+.1f dB)", g_master_volume, volume, gain,
                 gain > 0 ? 20 * std::log10(gain) : -INFINITY);
    g_master_volume = volume;
    g_muted = muted;
    g_master_target = gain;
    const uint32_t gen = g_master_gen.fetch_add(1) + 1;
    g_pending.push_back({NowMs(), gen, false, what});
}

void OnSelectModel(void *, const slp_select_model_t *m, const uint8_t *) {
    int amp = -1;
    if (m->kind == SLP_MODEL_BUILTIN && m->builtin_id >= 1 && m->builtin_id <= kAmpCount)
        amp = m->builtin_id;
    else if (m->kind == SLP_MODEL_BYPASS)
        amp = 0;
    if (m->kind == SLP_MODEL_UPLOADED) {
        Event("UI  SD profile %08x selected: not run (the Seed build doesn't take SD profiles yet)", m->hash);
        return;
    }
    if (amp < 0 || amp == g_requested_amp.load())
        return;
    g_requested_amp = amp;
    std::string what = "AMP -> bypass (no model; the amp's knobs do nothing while bypassed)";
    g_amp_switch_ms = NowMs();
    if (amp) {
        const ampctl::Voicing v = ampctl::VoicingForAmpId(amp);
        g_ctl.Configure(v, g_amp_knobs);
        what = std::string("AMP -> ") + AmpName(static_cast<AmpId>(amp)) + "  (its knobs:";
        for (int k = 0; k < ampctl::kSlots; ++k)
            what += std::string(" ") + ampctl::SlotLabel(v, k);
        what += ")";
    }
    const uint32_t gen = g_amp_gen.fetch_add(1) + 1;
    g_pending.push_back({NowMs(), gen, true, what});
}

void OnFx(void *, uint8_t fx, bool on, uint8_t model) {
    auto it = g_fx.find(fx);
    if (it != g_fx.end() && it->second == std::make_pair(on, static_cast<int>(model)))
        return;
    const bool first = it == g_fx.end();
    g_fx[fx] = {on, model};
    if (fx == SLP_FX_CAB) {
        g_requested_cab = on;
        if (!first)
            Event("UI  CAB %s  -> changes the sound (cabinet EQ)", on ? "on" : "off");
    } else if (!first && fx != SLP_FX_AMP) {
        Event("UI  %s %s, model %u  -> no effect on the sound (the Seed has no %s yet)", FxName(fx), on ? "on" : "bypassed", model, FxName(fx));
    }
}

void OnChain(void *, const uint8_t *fx, uint8_t len) {
    static std::string last;
    std::string s;
    for (int i = 0; i < len; ++i)
        s += std::string(i ? " > " : "") + FxName(fx[i]);
    if (s != last && !last.empty())
        Event("UI  chain order: %s > CAB  -> no effect on the sound (only the amp runs)", s.c_str());
    last = s;
}

bool IsAmpSlot(uint16_t id) { return id >= SLP_P_AMP_GAIN && id < SLP_P_AMP_GAIN + ampctl::kSlots; }

void OnParam(void *, uint16_t id, int16_t value) {
    if (IsAmpSlot(id)) {
        g_amp_knobs[id - SLP_P_AMP_GAIN] = value;
        const int amp = g_requested_amp.load();
        g_ctl.Configure(ampctl::VoicingForAmpId(amp ? amp : 1), g_amp_knobs);
        // The first full set (the snapshot at link up) is where the touchscreen already was, not a change.
        if (!g_amp_knobs_seen) {
            std::copy(g_amp_knobs, g_amp_knobs + ampctl::kSlots, g_amp_knobs_printed);
            if (++g_amp_knob_updates >= ampctl::kSlots)
                g_amp_knobs_seen = true;
        }
        return;
    }
    auto it = g_params.find(id);
    if (it != g_params.end() && it->second == value)
        return;
    const bool first = it == g_params.end();
    g_params[id] = value;
    if (!first)
        g_params_changed[id] = value;
}

void OnLink(void *, bool up) { Event("link %s", up ? "UP: the touchscreen is talking to the amp" : "LOST (keeping the current sound)"); }

double Db(double x) { return x > 1e-9 ? 20 * std::log10(x) : -120.0; }

void SigInt(int) { g_quit = true; }

} // namespace

// ---------------------------------------------------------------------------------------------
// --demo: renders before/after clips of each amp control into one WAV (no touchscreen needed) and
// measures what changed in the rendered audio, by band.

struct DemoClip {
    const char *title;
    int amp; // AmpId
    int16_t knobs[ampctl::kSlots];
};

// RMS level in three bands of a rendered clip (band-pass biquads), in dBFS.
struct Bands {
    double low, mid, high, all;
};
Bands Measure(const std::vector<float> &x) {
    auto band = [&](double f_lo, double f_hi) {
        // Two RBJ band-pass sections (0 dB at the centre) around the band's geometric centre.
        const double f0 = std::sqrt(f_lo * f_hi), q = f0 / (f_hi - f_lo), w = 2 * M_PI * f0 / kRate, al = std::sin(w) / (2 * q);
        const double a0 = 1 + al, b0 = al / a0, b2 = -al / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - al) / a0;
        double z[2][2] = {}, sq = 0;
        for (float v : x) {
            double y = v;
            for (auto &st : z) { // transposed direct form II, b1 = 0
                const double o = b0 * y + st[0];
                st[0] = -a1 * o + st[1];
                st[1] = b2 * y - a2 * o;
                y = o;
            }
            sq += y * y;
        }
        return 10 * std::log10(sq / x.size() + 1e-20);
    };
    double all = 0;
    for (float v : x)
        all += v * v;
    return {band(60, 250), band(250, 2000), band(2000, 8000), 10 * std::log10(all / x.size() + 1e-20)};
}

int RunDemo(const std::string &dir) {
    static const DemoClip clips[] = {
        {"Fender, all knobs at noon (as captured)", 1, {50, 50, 50, 50, 0}},
        {"Vox, all knobs at noon (as captured)", 2, {50, 50, 50, 50, 50}},
        {"Marshall, all knobs at noon (as captured)", 3, {50, 50, 50, 50, 50}},
        {"Marshall PREAMP 0 (clean)", 3, {0, 50, 50, 50, 50}},
        {"Marshall PREAMP 100 (driven hard)", 3, {100, 50, 50, 50, 50}},
        {"Fender TREBLE 0", 1, {50, 50, 50, 0, 0}},
        {"Fender TREBLE 100", 1, {50, 50, 50, 100, 0}},
        {"Fender BASS 0", 1, {50, 0, 50, 50, 0}},
        {"Fender BASS 100", 1, {50, 100, 50, 50, 0}},
        {"Fender MIDDLE 0 (deep scoop)", 1, {50, 50, 0, 50, 0}},
        {"Fender MIDDLE 100", 1, {50, 50, 100, 50, 0}},
        {"Fender VOLUME 20, BRIGHT off", 1, {20, 50, 50, 50, 0}},
        {"Fender VOLUME 20, BRIGHT on", 1, {20, 50, 50, 50, 100}},
        {"Vox CUT 0 (open)", 2, {50, 50, 0, 50, 50}},
        {"Vox CUT 100 (dark)", 2, {50, 50, 100, 50, 50}},
        {"Marshall PRESENCE 0", 3, {50, 50, 50, 50, 0}},
        {"Marshall PRESENCE 100", 3, {50, 50, 50, 50, 100}},
    };
    constexpr int kClipSeconds = 3, kGapSeconds = 1;
    std::vector<int16_t> tour;
    printf("Rendering %zu clips of %d s (Rocco's amp code + seed-dsp/amp_controls, simulated guitar).\n", std::size(clips), kClipSeconds);
    printf("    at  clip                                        level   low 60-250  mid 250-2k  high 2k-8k   (dBFS rms)\n");
    for (const DemoClip &c : clips) {
        NamAudio audio;
        audio.LoadAmpModel(static_cast<AmpId>(c.amp));
        audio.SetKnobs(1.0f, 1.0f);
        audio.ResetKnobs();
        ampctl::AmpControls ctl;
        ctl.Configure(ampctl::VoicingForAmpId(c.amp), c.knobs);
        Guitar guitar; // the same strum every clip
        std::vector<float> out;
        float in[kBlock], left[kBlock], right[kBlock];
        for (int b = 0; b < kRate * kClipSeconds / kBlock; ++b) {
            for (float &v : in)
                v = guitar.Next();
            ctl.ProcessPre(in, kBlock);
            audio.Process(in, left, right, kBlock, false);
            ctl.ProcessPost(left, kBlock);
            for (float v : left)
                out.push_back(std::clamp(v * 0.5f, -1.0f, 1.0f)); // master 70 (gain 0.49)
        }
        const Bands m = Measure(out);
        printf("  %4.0fs  %-42s %6.1f   %8.1f    %8.1f    %8.1f\n", tour.size() / double(kRate), c.title, m.all, m.low, m.mid, m.high);
        for (float v : out)
            tour.push_back(static_cast<int16_t>(v * 32767));
        tour.insert(tour.end(), kRate * kGapSeconds, 0);
    }
    const std::string path = dir + "/amp_demo.wav";
    WriteWav(path, tour);
    printf("saved %s (%.0f s). Listen: afplay %s\n", path.c_str(), tour.size() / double(kRate), path.c_str());
    return 0;
}

int main(int argc, char **argv) {
    const char *port = nullptr, *wav = nullptr;
    double seconds = 0;
    bool speakers = true;
    std::string out_dir = ".";
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[i + 1];
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--demo"))
            return RunDemo(out_dir);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)
            wav = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--no-speakers"))
            speakers = false;
        else
            port = argv[i];
    }
    if (!port) {
        fprintf(stderr, "usage: amp_sim /dev/cu.usbmodemXXXX [--input guitar.wav] [--seconds N] [--no-speakers] [--out DIR]\n");
        return 2;
    }
    if (!OpenPort(port)) {
        fprintf(stderr, "can't open %s at 1000000 baud\n", port);
        return 2;
    }
    std::vector<float> input;
    if (wav && !LoadWav(wav, input)) {
        fprintf(stderr, "can't read %s (48 kHz WAV, 16/24-bit PCM or 32-bit float)\n", wav);
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGINT, SigInt);

    // The receiver, configured like the Seed build but as a mock, so the touchscreen labels it.
    static slp_builtin_t builtins[kAmpCount];
    for (int i = 0; i < kAmpCount; ++i) {
        builtins[i].id = static_cast<uint8_t>(i + 1);
        snprintf(builtins[i].name, sizeof builtins[i].name, "%s", AmpName(static_cast<AmpId>(i + 1)));
    }
    slp_rx_config_t cfg{};
    cfg.send = PortSend;
    cfg.now_ms = LinkNow;
    cfg.boot_id = static_cast<uint32_t>(time(nullptr)) ^ 0x5eedu;
    cfg.fw_version = "amp-sim";
    cfg.sample_rate_hz = kRate;
    cfg.block_size = kBlock;
    cfg.builtins = builtins;
    cfg.builtin_count = kAmpCount;
    cfg.role = SLP_ROLE_MOCK_SEED;
    cfg.on_master = OnMaster;
    cfg.on_select_model = OnSelectModel;
    cfg.on_fx = OnFx;
    cfg.on_chain = OnChain;
    cfg.on_param = OnParam;
    cfg.on_link = OnLink;
    static slp_receiver_t rx;
    slp_rx_init(&rx, &cfg);

    // Audio: the Mac's speakers through an AudioQueue (10 ms buffers), or a paced thread.
    static Engine engine(std::move(input));
    g_engine = &engine;
    AudioQueueRef queue = nullptr;
    std::thread paced;
    if (speakers) {
        AudioStreamBasicDescription f{};
        f.mSampleRate = kRate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 4;
        f.mFramesPerPacket = f.mChannelsPerFrame = 1;
        f.mBitsPerChannel = 32;
        if (AudioQueueNewOutput(&f, AudioQueueCallback, nullptr, nullptr, nullptr, 0, &queue) != noErr) {
            fprintf(stderr, "no audio output; continuing without speakers\n");
            speakers = false;
        } else {
            for (int i = 0; i < 3; ++i) {
                AudioQueueBufferRef b;
                AudioQueueAllocateBuffer(queue, kBlock * 10 * sizeof(float), &b);
                AudioQueueCallback(nullptr, queue, b);
            }
            AudioQueueStart(queue, nullptr);
        }
    }
    if (!speakers) {
        paced = std::thread([] {
            float out[kBlock];
            auto next = std::chrono::steady_clock::now();
            while (!g_quit) {
                g_engine->Block(out);
                next += std::chrono::microseconds(static_cast<int>(kBlockUs));
                std::this_thread::sleep_until(next);
            }
        });
    }

    printf("amp_sim on %s: Rocco's amp code, %s, %s. Ctrl-C to stop.\n", port, wav ? wav : "simulated guitar (E5 A5 D5 G5)",
           speakers ? "playing on the Mac's speakers (start with the Mac's volume low)" : "speakers off");
    printf("     time  what the touchscreen changed\n");

    double next_status = 0, next_meters = 0, next_line = 1000, next_params = 0;
    uint64_t last_blocks = 0;
    while (!g_quit && (seconds <= 0 || NowMs() < seconds * 1000)) {
        // Link.
        uint8_t buf[512];
        pollfd p{g_fd, POLLIN, 0};
        if (poll(&p, 1, 2) > 0) {
            const ssize_t n = read(g_fd, buf, sizeof buf);
            if (n > 0)
                slp_rx_feed(&rx, buf, static_cast<size_t>(n));
        }
        slp_rx_poll(&rx);
        const double now = NowMs();

        // Changes that reached the audio: print with how long it took.
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            const bool done = it->amp ? g_amp_applied_gen.load() >= it->gen : g_master_applied_gen.load() >= it->gen;
            if (!done) {
                ++it;
                continue;
            }
            const double applied = it->amp ? g_amp_applied_ms.load() : g_master_applied_ms.load();
            const bool failed = it->amp && !g_model_ok;
            Event("UI  %s  -> changes the sound; in the audio %.1f ms after arriving%s", it->what.c_str(), applied - it->received_ms,
                  it->amp ? " (includes loading the model)" : "");
            if (failed)
                Event("    model FAILED to load; audio is bypassed");
            it = g_pending.erase(it);
        }
        // Amp knob changes: grouped like the others, each with what it did to the sound.
        if (now >= next_params) {
            const int amp = g_active_amp.load();
            const ampctl::Voicing v = ampctl::VoicingForAmpId(amp ? amp : 1);
            // An amp switch loads that amp's knob positions in the same change: not knob turns.
            if (now - g_amp_switch_ms < 200)
                std::copy(g_amp_knobs, g_amp_knobs + ampctl::kSlots, g_amp_knobs_printed);
            for (int k = 0; k < ampctl::kSlots; ++k) {
                if (g_amp_knobs[k] == g_amp_knobs_printed[k])
                    continue;
                int16_t before[ampctl::kSlots];
                std::copy(g_amp_knobs_printed, g_amp_knobs_printed + ampctl::kSlots, before);
                ampctl::AmpControls a, b;
                a.Configure(v, before);
                before[k] = g_amp_knobs[k];
                b.Configure(v, before);
                char effect[160];
                if (amp == 0) {
                    snprintf(effect, sizeof effect, "no effect while the amp is bypassed");
                } else if (k == 0) {
                    snprintf(effect, sizeof effect, "changes the sound: drives the amp %+.1f dB (%s breakup)", b.DriveDb() - a.DriveDb(),
                             b.DriveDb() > a.DriveDb() ? "more" : "less");
                } else if (ampctl::SlotIsSwitch(v, k)) {
                    snprintf(effect, sizeof effect, "changes the sound: bright cap %s (treble lift, strongest at low volume)",
                             g_amp_knobs[k] >= 50 ? "ON" : "OFF");
                } else {
                    snprintf(effect, sizeof effect, "changes the sound: %+.1f dB at 100 Hz, %+.1f at 500 Hz, %+.1f at 2.5 kHz, %+.1f at 5 kHz",
                             b.PostResponseDb(100) - a.PostResponseDb(100), b.PostResponseDb(500) - a.PostResponseDb(500),
                             b.PostResponseDb(2500) - a.PostResponseDb(2500), b.PostResponseDb(5000) - a.PostResponseDb(5000));
                }
                Event("UI  %s %d -> %d  -> %s", ampctl::SlotLabel(v, k), g_amp_knobs_printed[k], g_amp_knobs[k], effect);
                g_amp_knobs_printed[k] = g_amp_knobs[k];
                next_params = now + 300;
            }
        }
        // Other knob changes: grouped, since a slider drag sends many.
        if (now >= next_params && !g_params_changed.empty()) {
            std::string s;
            for (auto &[id, v] : g_params_changed)
                s += std::string(s.empty() ? "" : ", ") + ParamName(id) + " " + std::to_string(v);
            Event("UI  %s  -> no effect on the sound (the Seed has no DSP for %s yet)", s.c_str(),
                  g_params_changed.size() == 1 ? "it" : "these");
            g_params_changed.clear();
            next_params = now + 300;
        }

        // Telemetry, like the Seed: STATUS twice a second, METERS about 30 times a second.
        if (slp_rx_link_up(&rx) && now >= next_status) {
            next_status = now + 500;
            const uint64_t blocks = g_blocks.load();
            const double avg = blocks > last_blocks ? g_proc_us.exchange(0) / static_cast<double>(blocks - last_blocks) : 0;
            last_blocks = blocks;
            const int amp = g_active_amp.load();
            slp_status_t st{};
            st.cpu_avg_x10 = static_cast<uint16_t>(avg / kBlockUs * 1000);
            st.cpu_peak_x10 = static_cast<uint16_t>(g_proc_max_us.exchange(0) / kBlockUs * 1000);
            st.overruns = g_overruns.load();
            st.model_kind = amp == 0 ? SLP_MODEL_BYPASS : SLP_MODEL_BUILTIN;
            st.builtin_id = static_cast<uint8_t>(amp);
            st.flags = SLP_STATUS_AUDIO_RUNNING | (amp == 0 ? SLP_STATUS_BYPASS : 0) | (amp && !g_model_ok ? SLP_STATUS_MODEL_FAILED : 0);
            slp_rx_send_status(&rx, &st);
        }
        if (slp_rx_link_up(&rx) && now >= next_meters) {
            next_meters = now + 33;
            auto cdb = [](float x) { return x > 1e-6f ? static_cast<int16_t>(2000.0f * std::log10(x)) : static_cast<int16_t>(SLP_METER_SILENCE); };
            slp_meters_t m{};
            m.in_peak_cdb = cdb(g_in_peak.exchange(0));
            m.out_peak_cdb = cdb(g_out_peak.exchange(0));
            m.clip_count = g_clips.load();
            slp_rx_send_meters(&rx, &m);
        }

        // Once a second: what's playing and how loud.
        if (now >= next_line) {
            next_line += 1000;
            const double n = static_cast<double>(std::max<uint64_t>(1, g_level_samples.exchange(0)));
            const double in_rms = std::sqrt(g_in_sq.exchange(0) / n), out_rms = std::sqrt(g_out_sq.exchange(0) / n);
            const int amp = g_active_amp.load();
            std::string knobs;
            if (amp)
                for (int k = 0; k < ampctl::kSlots; ++k)
                    knobs += std::string(" ") + ampctl::SlotLabel(ampctl::VoicingForAmpId(amp), k) + " " + std::to_string(g_amp_knobs[k]);
            printf("%9.3f  [%s%s | master %s | cab %s]  guitar %5.1f dBFS rms -> out %5.1f dBFS rms   link %s\n", now / 1000.0,
                   amp ? AmpName(static_cast<AmpId>(amp)) : "bypass", knobs.c_str(),
                   g_master_volume == 255 ? "100 (no touchscreen yet)" : (g_muted ? "MUTED" : std::to_string(g_master_volume).c_str()),
                   g_requested_cab.load() ? "on" : "off", Db(in_rms), Db(out_rms), slp_rx_link_up(&rx) ? "up" : "waiting");
        }
    }

    // Stop and save the recording.
    g_quit = true;
    if (queue)
        AudioQueueDispose(queue, true);
    if (paced.joinable())
        paced.join();
    const std::string path = out_dir + "/amp_sim_output.wav";
    WriteWav(path, engine.Recording());
    printf("saved %.1f s of output to %s\n", engine.Recording().size() / static_cast<double>(kRate), path.c_str());
    printf("link: %lu frames received, %lu dropped\n", static_cast<unsigned long>(rx.dec.frames_ok),
           static_cast<unsigned long>(slp_decoder_errors(&rx.dec)));
    close(g_fd);
    return 0;
}
