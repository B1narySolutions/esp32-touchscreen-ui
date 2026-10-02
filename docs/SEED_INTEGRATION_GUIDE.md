# Seed integration guide

For whoever owns `realtime-nam-seed3`: how to make the Daisy Seed3 talk to the touchscreen.
The touchscreen side is finished and tested against a simulated Seed; nothing in your firmware
has been changed. Everything below is a proposal for your code, and you decide how it lands.

- Protocol reference: [SEED_LINK_PROTOCOL.md](SEED_LINK_PROTOCOL.md) (byte layouts, hex examples).
- What has and hasn't been verified: [SEED_LINK_STATUS.md](SEED_LINK_STATUS.md).
- Bring-up steps for the lab: [LAB_DAY_CHECKLIST.md](LAB_DAY_CHECKLIST.md).

## 1. What the Seed has to do

1. Receive bytes on USART1 with DMA, and hand them to the receiver in the main loop.
2. Apply what the touchscreen sends: master volume and mute (most important), amp model
   selection, and parameter values for the parts of the chain that exist on the Seed.
3. Report back: a STATUS line twice a second (DSP load, overruns, active model) and input/output
   peak meters about 30 times a second.
4. Accept uploaded A2-Lite models (7484 bytes, CRC-checked) and run them like the built-ins.

The ESP32 does the hard parts: JSON parsing, `.nam` validation and packing (an exact port of
`scripts/convert_a2.py`, byte-identical on your three models), coalescing, retries and
resynchronisation. The Seed only decodes small binary frames.

## 2. Wiring

Carrier J4 "DISPLAY_UART" (pin order traced from `seed3-carrier.kicad_pcb`) to the touchscreen's
4-pin header H3 "UART2":

| ESP32 board H3 | Seed carrier J4 | Signal |
|---|---|---|
| pin 2 GND | pin 1 GND | ground |
| pin 3 RX (GPIO44) | pin 2 /DISPLAY_UART_TX (Seed D13, USART1_TX, PB6) | Seed to ESP32 |
| pin 4 TX (GPIO43) | pin 3 /DISPLAY_UART_RX (Seed D14, USART1_RX, PB7) | ESP32 to Seed |
| pin 1 3V3 | **not connected** | |
| **not connected** | pin 4 +5V | |

> **J4 pin 4 is +5 V and H3 pin 1 is the ESP32's 3.3 V rail. A straight 4-wire cable connects
> them and can damage the ESP32 board. Use three wires (GND, and the two crossed signals).**

Both sides are 3.3 V logic; no level shifting is needed. The ESP32 board's UART selection
switch (SW1) must be on **UART2** to route GPIO43/44 to H3 (on UART1 they go to its USB TO UART
port instead, which is how the simulated Seed is connected).

Link settings: **1,000,000 baud, 8N1, no flow control.**

## 3. Files to copy

From `esp32-touchscreen-ui/firmware/main/seed_link_proto/` into your tree (they are C99 with
`extern "C"` guards, no allocation, no OS, no libDaisy dependency):

| File | What it is |
|---|---|
| `seed_link_proto.h/.c` | Framing (COBS + CRC-16), CRC-32, every message's pack/unpack |
| `slp_receiver.h/.c` | The Seed side of the protocol: handshake, heartbeat, PING/PONG, state hooks, snapshot confirmation, model upload into your buffer |
| `slp_params.h` | Named parameter ids (`SLP_P_AMP_GAIN` ...), generated; map them by meaning |

In the libDaisy Makefile, add them as C sources (they compile as C, your code stays C++20):

```make
C_SOURCES += src/link/seed_link_proto.c src/link/slp_receiver.c
C_INCLUDES += -Isrc/link
```

`slp_receiver.c` is covered by host tests (tests/host/test_receiver.c), and the touchscreen's
real firmware has been run against it on a PC serial port (tests/host/seed_sim.c).

## 4. UART with DMA (libDaisy)

The libDaisy calls below follow its `UartHandler` API, but **the touchscreen team could not
compile against your pinned libDaisy (`cc146d5`)**; check the names against
`libs/libDaisy/src/per/uart.h` before relying on them.

```cpp
#include "daisy_seed.h"
#include "slp_receiver.h"

using namespace daisy;

static UartHandler link_uart;

// The DMA buffer must be in DMA-reachable memory (SRAM D2): libDaisy's DMA_BUFFER_MEM_SECTION.
static uint8_t DMA_BUFFER_MEM_SECTION link_dma_buf[256];

// Single-producer / single-consumer ring between the DMA callback and the main loop.
static uint8_t link_ring[2048];
static std::atomic<uint32_t> ring_head{0}, ring_tail{0};

// Runs in the UART/DMA interrupt: copy bytes only. Never decode or touch audio state here.
static void LinkRx(uint8_t *data, size_t size, void *, UartHandler::Result result) {
    if (result != UartHandler::Result::OK) return;
    uint32_t head = ring_head.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size; ++i) {
        const uint32_t next = (head + 1) % sizeof(link_ring);
        if (next == ring_tail.load(std::memory_order_acquire)) break; // full: drop (the protocol resyncs)
        link_ring[head] = data[i];
        head = next;
    }
    ring_head.store(head, std::memory_order_release);
}

static void InitLink() {
    UartHandler::Config cfg;
    cfg.periph = UartHandler::Config::Peripheral::USART_1;
    cfg.mode = UartHandler::Config::Mode::TX_RX;
    cfg.baudrate = 1000000;
    cfg.pin_config.tx = seed::D13;   // PB6
    cfg.pin_config.rx = seed::D14;   // PB7
    link_uart.Init(cfg);
    link_uart.DmaListenStart(link_dma_buf, sizeof(link_dma_buf), LinkRx, nullptr);
}
```

Main loop (your existing loop in `main.cpp` already runs `ApplyPendingAmpCommand` and
`ReportStatus` here):

```cpp
static slp_receiver_t link;

static void LinkSend(void *, const uint8_t *bytes, size_t len) {
    link_uart.BlockingTransmit(const_cast<uint8_t *>(bytes), len); // at most ~270 bytes, ~3 ms
}
static uint32_t LinkNow(void *) { return System::GetNow(); }

static void PollLink() {
    uint8_t buf[256];
    size_t n = 0;
    uint32_t tail = ring_tail.load(std::memory_order_relaxed);
    while (tail != ring_head.load(std::memory_order_acquire) && n < sizeof(buf)) {
        buf[n++] = link_ring[tail];
        tail = (tail + 1) % sizeof(link_ring);
    }
    ring_tail.store(tail, std::memory_order_release);
    if (n) slp_rx_feed(&link, buf, n);
    slp_rx_poll(&link);
}
```

## 5. Configuring the receiver

```cpp
static const slp_builtin_t kBuiltins[] = {
    {1, "Fender Twin65"}, {2, "Vox AC30 Chimey"}, {3, "Marshall JCM800 G5"},  // AmpId order
};
static uint8_t staging_weights[A2Lite::kWeights * 4];  // uploads land here

static void InitReceiver(uint32_t boot_id) {
    slp_rx_config_t cfg{};
    cfg.send = LinkSend;
    cfg.now_ms = LinkNow;
    cfg.boot_id = boot_id;               // different every boot (RNG, or a timer read at startup)
    cfg.fw_version = "nam-a2";
    cfg.sample_rate_hz = 48000;
    cfg.block_size = 48;
    cfg.builtins = kBuiltins;
    cfg.builtin_count = 3;
    cfg.model_buf = staging_weights;
    cfg.model_buf_size = sizeof(staging_weights);
    cfg.on_master = OnMaster;
    cfg.on_select_model = OnSelectModel;
    cfg.on_param = OnParam;              // optional until effects exist
    cfg.on_fx = OnFx;                    // includes amp on/bypass
    slp_rx_init(&link, &cfg);
}
```

The names in `kBuiltins` are what the touchscreen shows in its AMP picker, so build them from
`AmpName()` / `models/amps.json` rather than typing them twice.

### Hooks: what each should do

All hooks run in the main loop (inside `slp_rx_feed`). Hand values to the audio callback through
atomics, as the existing `requested_amp` does.

| Hook | Recommended behaviour |
|---|---|
| `on_master(volume, muted)` | Store a target gain. **Mute must take effect immediately** (ramp to silence within a few ms, not over a long fade). Keep the volume while muted, so un-mute restores it. Suggested curve: 0 = silent, otherwise `gain_db = -60 + 60 * (volume / 100)^0.5`, or whatever suits the output stage. |
| `on_select_model(m, weights)` | `SLP_MODEL_BUILTIN`: request `m->builtin_id` exactly like the USB key '1'..'3' (reuse `ApplyPendingAmpCommand`). `SLP_MODEL_BYPASS`: like key '0'. `SLP_MODEL_UPLOADED` with `weights` non-null: see section 6. With `weights == NULL`: keep the current model and set `SLP_STATUS_MODEL_FAILED` in STATUS (the touchscreen uploads it again). |
| `on_param(id, value)` | Values are 0..100 in UI units. Map by id (`slp_params.h`, meanings in the protocol table). Smooth them in the audio callback (section 7). Ignore ids for effects that don't exist yet. |
| `on_fx(fx, on, model)` | On/bypass per effect, plus the UI's model choice for that effect. For `SLP_FX_AMP`, `on == false` means amp bypass; the authoritative amp model is `on_select_model`. |
| `on_chain(fx, len)` | Processing order of the effects; CAB is always last. Only matters once the Seed runs more than the amp. |
| `on_link(up)` | Informational. **On link loss keep the current sound**: don't mute, don't reset, don't jump to defaults. |

Never call into the audio engine from a hook directly. Changing the model means stopping audio,
as `ApplyPendingAmpCommand` already does.

### Telemetry

```cpp
// About twice a second, from the same counters ReportStatus() prints:
slp_status_t st{};
st.cpu_avg_x10 = avg_us * 1000 / kBlockBudgetUs;   // 0.1 % of the block budget
st.cpu_peak_x10 = max_us * 1000 / kBlockBudgetUs;
st.overruns = total_overruns_since_boot;
st.model_kind = active_kind;                       // SLP_MODEL_*
st.builtin_id = static_cast<uint8_t>(selected_amp);
st.model_hash = active_uploaded_crc;               // for SLP_MODEL_UPLOADED, else 0
st.flags = (bypass ? SLP_STATUS_BYPASS : 0) | (model_ready ? 0 : SLP_STATUS_MODEL_FAILED) | SLP_STATUS_AUDIO_RUNNING;
slp_rx_send_status(&link, &st);                    // fills in applied_snapshot_id itself

// About 30 times a second: peaks since the last call, collected in the audio callback.
slp_meters_t m{};
m.in_peak_cdb = PeakToCentiDb(in_peak.exchange(0.0f));   // -600 = -6.00 dBFS, -12000 = silence
m.out_peak_cdb = PeakToCentiDb(out_peak.exchange(0.0f));
m.clip_count = clips.load();
slp_rx_send_meters(&link, &m);
```

`PeakToCentiDb(x)` is `x > 1e-6f ? (int16_t)(2000.0f * log10f(x)) : SLP_METER_SILENCE`. The audio
callback only does `in_peak = max(in_peak, |sample|)` style updates; the logarithm runs in the
main loop. The touchscreen's IN/OUT meters show nothing until METERS arrive (it never invents
levels), so these are what make them move.

## 6. Uploaded models

The receiver writes an upload into `model_buf`, checks its CRC-32 against the touchscreen's,
and only then answers MODEL_RESULT OK. The bytes are little-endian float32 already in your
engine's packed order (what `convert_a2.py pack()` produces), 1871 weights.

`model_buf` is a **staging** buffer: the next upload overwrites it. Your engine keeps pointers
into its weights (`A2Lite` stores `const float*` views), so when `on_select_model` delivers an
uploaded model:

1. Stop audio (as for a built-in switch).
2. Copy the 7484 bytes into a separate buffer that lives as long as the model, e.g.
   `static float uploaded_weights[A2Lite::kWeights];`.
3. Build the model on that buffer (`std::make_unique<A2Lite>(uploaded_weights)`, the same path
   `CreateAmpModel` takes for embedded weights) and `Reset()` it.
4. Restart audio, and report the result through STATUS (`model_kind = SLP_MODEL_UPLOADED`,
   `model_hash = <crc32>`, or `SLP_STATUS_MODEL_FAILED`).

After a Seed reboot nothing is held; the touchscreen notices the new `boot_id` and uploads the
selected model again automatically (about 0.1 s).

## 7. Avoiding zipper noise

Parameter changes arrive at most every 10 ms (coalesced), as steps of 1 % or more. Apply them
through a one-pole smoother per parameter in the audio callback, e.g. a time constant of
about 10 to 20 ms (`y += a * (target - y)` per sample or per block, with
`a = 1 - exp(-1 / (tau * rate))`). Master volume especially should be smoothed; mute should
reach silence within about 5 ms.

## 8. Rules that keep audio safe

- Decode only in the main loop. The DMA callback copies bytes; the audio callback reads atomics.
- No allocation in the audio callback (as today).
- Model changes stop audio first (as today).
- The touchscreen re-sends the full state on every Seed HELLO, after a preset load, every 5 s,
  and within about a second of a lost frame. A Seed that missed something is repaired without
  any special handling.
