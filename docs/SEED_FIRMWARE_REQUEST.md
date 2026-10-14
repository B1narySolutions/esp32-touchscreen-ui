# Request for the Seed firmware: talk to the touchscreen

For the owner of `realtime-nam-seed3` (Rocco). Short version of
[SEED_INTEGRATION_GUIDE.md](SEED_INTEGRATION_GUIDE.md), written against your `main` at `c3abe82` (with your volume and tone knobs).
Nothing in your repo has been changed; this is a proposal and you decide how it lands.

## Why

The touchscreen is finished and talks to the Seed over a UART: it sends master volume, mute,
the amp choice and the cabinet on/off, and shows the Seed's DSP load, active model and IN/OUT
meters. It has been tested for hours against a simulated Seed on a PC. Your firmware has no
UART link yet, so on the real Seed the touchscreen just says "Not connected". The receiver code
that does the protocol is already written for you (plain C99, no allocation, no libDaisy
dependency, host-tested): you only have to feed it bytes and act on what it tells you.

## Wiring (bare Seed)

Three wires, both boards powered off while wiring:

| Seed | Touchscreen header H3 "UART2" |
|---|---|
| D13 (pin 14, USART1 TX) | pin 3 (RX) |
| D14 (pin 15, USART1 RX) | pin 4 (TX) |
| DGND (pin 40) | pin 2 (GND) |

Nothing on H3 pin 1 (3V3). The touchscreen's switch SW1 goes to **UART2**. Link: **1,000,000 baud,
8N1, no flow control.** Your firmware doesn't use D13/D14 today.

## What to add (phase 1: no model uploads yet)

**Shortcut:** steps 2 and 3 below are also a ready patch, [seed3-link.patch](seed3-link.patch).
From your repo root, after step 1:

```sh
git apply path/to/seed3-link.patch
```

It applies cleanly to your `main` at `c3abe82` and is `clang-format` clean with your `.clang-format`.

**Tested on a real Seed (2026-10-13, bare Seed, touchscreen wired to D13/D14/DGND).** Your firmware plus this
patch compiles with no new warnings (Arm GNU 15.3.rel2, libDaisy `cc146d5`); flash goes from 108,700 B (82.9 %) to
117,184 B (89.4 %). On the Seed: the link came up in about a second, every amp card loaded the amp (`>>> ... loaded`),
mute took the output meter to silence, ping 1.95 ms, Seed reset and cable pulls recovered by themselves, 0 link
errors, and your status line kept 0 overruns with DSP load 57 % (56 % without the patch). This was a test build on
the team's Seed; your original firmware was read off it first, byte for byte, and goes back on after testing.
Nothing in your repo was changed.

**One thing you'd hit without the patch: a libDaisy UART lock-up.** On any line error (the touchscreen prints a
115200-baud boot message on the same pin, which arrives as garbage at 1 Mbaud), libDaisy's `HAL_UART_ErrorCallback`
leaves listen mode but keeps the IDLE interrupt enabled, and its `UART_IRQHandler` only clears the IDLE flag in
listen mode. USART1 then interrupts forever and your main loop (and USB) stops: no status line, no answer to `B`,
BOOT + RESET needed. The patch's `StartLinkListening()` turns the line-error interrupts off after
`DmaListenStart` (a damaged byte still arrives and the protocol's CRC drops its frame), and `PollLink()` clears
the error flags and restarts listening if it ever stopped. Keep that part if you write it differently.

### 1. Copy five files

From `esp32-touchscreen-ui/firmware/main/seed_link_proto/` into `src/link/`:
`seed_link_proto.h`, `seed_link_proto.c`, `slp_receiver.h`, `slp_receiver.c`, `slp_params.h`.
Please keep them byte-identical (both sides must agree on the protocol); if something in them
needs changing, tell us rather than editing your copy.

### 2. `make/firmware.mk`

```make
C_SOURCES = src/link/seed_link_proto.c src/link/slp_receiver.c
```

before the `include $(SYSTEM_FILES_DIR)/Makefile` line (libDaisy's core Makefile appends to
`C_SOURCES` and compiles them as C). Your existing `-Isrc` already finds `link/slp_receiver.h`.

### 3. `src/main.cpp`

This is the code in the patch, in pieces (`seed::D13` is PB6, `seed::D14` is PB7 in your pinned
libDaisy). Compiled, not yet run on hardware.

**UART with DMA into a ring buffer** (the DMA callback only copies bytes):

```cpp
#include "link/slp_receiver.h"

static daisy::UartHandler link_uart;
// DMA can only reach some RAM; libDaisy's section macro puts the buffer there.
static uint8_t DMA_BUFFER_MEM_SECTION link_dma_buf[256];
// Single-producer / single-consumer ring between the DMA callback and the main loop.
static uint8_t link_ring[2048];
static std::atomic<uint32_t> ring_head{0}, ring_tail{0};

// UART DMA interrupt: copy bytes only. If the ring is full, drop them; the protocol resyncs.
static void OnLinkRx(uint8_t *data, size_t size, void *, daisy::UartHandler::Result result) {
    if (result != daisy::UartHandler::Result::OK)
        return;
    uint32_t head = ring_head.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size; ++i) {
        const uint32_t next = (head + 1) % sizeof(link_ring);
        if (next == ring_tail.load(std::memory_order_acquire))
            break;
        link_ring[head] = data[i];
        head = next;
    }
    ring_head.store(head, std::memory_order_release);
}
```

**Master volume / mute applied in the audio callback.** With no touchscreen connected the
defaults leave the sound exactly as today (gain 1):

```cpp
static std::atomic<float> master_target{1.0f};  // set by the link, read by the audio callback
static float master_gain = 1.0f;                // audio callback only
static std::atomic<float> in_peak{0.0f}, out_peak{0.0f};
static std::atomic<uint32_t> clip_count{0};
static_assert(std::atomic<float>::is_always_lock_free, "Audio atomics must be lock-free");

// In AudioCallback, after audio.Process(...): ramp to the target across the block (no clicks,
// and mute reaches silence within one 1 ms block), and record peaks for the meters.
const float target = master_target.load(std::memory_order_relaxed);
const float step = (target - master_gain) / static_cast<float>(frame_count);
float in_max = 0.0f, out_max = 0.0f;
for (size_t i = 0; i < frame_count; ++i) {
    master_gain += step;
    output_channels[0][i] *= master_gain;
    output_channels[1][i] *= master_gain;
    in_max = std::max(in_max, std::fabs(input_channels[0][i]));
    out_max = std::max(out_max, std::fabs(output_channels[0][i]));
}
master_gain = target;  // no drift from float rounding
if (out_max >= 1.0f)
    clip_count.fetch_add(1, std::memory_order_relaxed);
RaisePeak(in_peak, in_max);   // compare_exchange loop like RecordCallbackTiming's peak
RaisePeak(out_peak, out_max);
```

**The receiver and its hooks** (hooks run in the main loop, inside `slp_rx_feed`):

```cpp
static slp_receiver_t link_rx;  // not "link": that name is taken by newlib's link()
static const slp_builtin_t kBuiltins[] = {
    {1, "Fender Twin65"}, {2, "Vox AC30 Chimey"}, {3, "Marshall JCM800 G5"},  // AmpId order; better built from AmpName()
};

static void LinkSend(void *, const uint8_t *bytes, size_t len) {
    // At most ~270 bytes, under 3 ms at 1 Mbaud; audio keeps running in its interrupt.
    link_uart.BlockingTransmit(const_cast<uint8_t *>(bytes), len);
}
static uint32_t LinkNow(void *) { return daisy::System::GetNow(); }

// Volume is 0..100. A squared curve is close to an audio taper and needs no libm.
static void OnMaster(void *, uint8_t volume, bool muted) {
    const float v = volume / 100.0f;
    master_target.store(muted ? 0.0f : v * v, std::memory_order_relaxed);
}

// Same path as the USB keys: record the request; ApplyPendingAmpCommand does the switch.
static void OnSelectModel(void *, const slp_select_model_t *m, const uint8_t *) {
    if (m->kind == SLP_MODEL_BUILTIN && m->builtin_id >= 1 && m->builtin_id <= kAmpCount)
        requested_amp.store(m->builtin_id, std::memory_order_relaxed);
    else if (m->kind == SLP_MODEL_BYPASS)
        requested_amp.store(0, std::memory_order_relaxed);
    // SLP_MODEL_UPLOADED (an SD profile): phase 2. Uploads are refused while model_buf is null.
}

static void OnFx(void *, uint8_t fx, bool on, uint8_t) {
    if (fx == SLP_FX_CAB)
        requested_cabinet.store(on, std::memory_order_relaxed);
}

static void OnLink(void *, bool up) { seed.PrintLine(">>> touchscreen link %s", up ? "up" : "lost"); }

static void InitLink() {
    // UART1 on D13/D14 at 1 Mbaud, received by DMA.
    daisy::UartHandler::Config uart{};
    uart.periph = daisy::UartHandler::Config::Peripheral::USART_1;
    uart.mode = daisy::UartHandler::Config::Mode::TX_RX;
    uart.baudrate = 1000000;
    uart.pin_config.tx = daisy::seed::D13;
    uart.pin_config.rx = daisy::seed::D14;
    link_uart.Init(uart);
    StartLinkListening(); // DmaListenStart with line-error interrupts off; see the lock-up note above

    // The receiver: a new boot_id every boot tells the touchscreen to resend everything.
    daisy::Random::Init();
    slp_rx_config_t cfg{};
    cfg.send = LinkSend;
    cfg.now_ms = LinkNow;
    cfg.boot_id = daisy::Random::GetValue();
    cfg.fw_version = "nam-a2";
    cfg.sample_rate_hz = 48000;
    cfg.block_size = NamAudio::kBlockSize;
    cfg.builtins = kBuiltins;
    cfg.builtin_count = kAmpCount;
    cfg.on_master = OnMaster;
    cfg.on_select_model = OnSelectModel;
    cfg.on_fx = OnFx;
    cfg.on_link = OnLink;
    slp_rx_init(&link_rx, &cfg);
}

// Every main-loop pass: hand the received bytes to the receiver.
static void PollLink() {
    // Clear line-error flags left by damaged bytes, and listen again should reception ever stop.
    USART1->ICR = USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF | USART_ICR_PECF;
    if (!link_uart.IsListening()) {
        link_uart.DmaListenStop();
        StartLinkListening();
    }

    uint8_t buf[256];
    size_t n = 0;
    uint32_t tail = ring_tail.load(std::memory_order_relaxed);
    while (tail != ring_head.load(std::memory_order_acquire) && n < sizeof(buf)) {
        buf[n++] = link_ring[tail];
        tail = (tail + 1) % sizeof(link_ring);
    }
    ring_tail.store(tail, std::memory_order_release);
    if (n)
        slp_rx_feed(&link_rx, buf, n);
    slp_rx_poll(&link_rx);
}
```

**Telemetry.** STATUS about twice a second with the numbers `ReportStatus` already measures
(keep its last `avg_us`/`max_us` and a running overrun total in `AppState`), METERS about 30
times a second. The touchscreen shows nothing for DSP load or meters until these arrive; it
never makes values up.

```cpp
// STATUS every 500 ms.
slp_status_t st{};
st.cpu_avg_x10 = state.last_avg_us * 1000 / kBlockBudgetUs;
st.cpu_peak_x10 = state.last_max_us * 1000 / kBlockBudgetUs;
st.overruns = state.total_overruns;                 // since boot, not per second
st.model_kind = bypass ? SLP_MODEL_BYPASS : SLP_MODEL_BUILTIN;
st.builtin_id = bypass ? 0 : static_cast<uint8_t>(state.selected_amp);
st.flags = SLP_STATUS_AUDIO_RUNNING | (bypass ? SLP_STATUS_BYPASS : 0) | (!bypass && !state.model_ready ? SLP_STATUS_MODEL_FAILED : 0);
slp_rx_send_status(&link_rx, &st);

// METERS every 33 ms. (log10f measured at about 0.3 KB of flash.)
auto centi_db = [](float x) { return x > 1e-6f ? static_cast<int16_t>(2000.0f * log10f(x)) : SLP_METER_SILENCE; };
slp_meters_t m{};
m.in_peak_cdb = centi_db(in_peak.exchange(0.0f));
m.out_peak_cdb = centi_db(out_peak.exchange(0.0f));
m.clip_count = clip_count.load();
slp_rx_send_meters(&link_rx, &m);
```

`main()`: call `InitLink()` after `InitHardware()`, and `PollLink()` plus the telemetry timer in
the `while (true)` loop next to `ApplyPendingAmpCommand`.

## Things to know

- **The touchscreen wins over the USB keys.** It resends its full state every 5 s, so a `1`/`2`/`3`
  typed on the Seed's USB console is undone within 5 s while the touchscreen is connected.
- **Link loss keeps the sound.** `OnLink(false)` should not mute or reset anything.
- **Two volume controls.** The patch applies the touchscreen's MASTER after `audio.Process`, so it multiplies
  with your volume knob. Decide whether that's what you want, or whether MASTER should drive `OutputControls`.
- **Knobs on a bare Seed.** With nothing on A0/A1 your knob readings wander (53-100 % in our test), so the output
  level changes by itself. On the carrier this doesn't happen.
- **Phase 2, SD profiles:** the touchscreen can upload TONE3000 A2 models (7484 bytes, CRC-checked)
  from its SD card. That needs a staging buffer (`cfg.model_buf`) and loading the weights the way
  `CreateAmpModel` does; see section 6 of SEED_INTEGRATION_GUIDE.md. Until then, picking an SD
  profile on the touchscreen says "Upload failed ... Retrying" and the Seed keeps its amp.

## How to check it with the touchscreen

1. Seed console: `>>> touchscreen link up` within a second of both running.
2. Touchscreen Test Mode, SEED3 panel: **Connected** (green), firmware "nam-a2", 48000 Hz / 48,
   DSP load equal to your console's `avg`, model name equal to yours.
3. Tap MUTE: silence at once; un-mute: same level. Drag MASTER: smooth.
4. Tap each AMP card: your console prints `>>> ... loaded`.
5. Play: the touchscreen's IN/OUT meters move.
6. Your status line: DSP load and overruns the same with the cable plugged in as without.

Any problem or question: the protocol is in SEED_LINK_PROTOCOL.md; the touchscreen side
(jasm4916) can run the link against a PC stand-in to reproduce anything you see.
