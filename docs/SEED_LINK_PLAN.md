# Seed link plan

Connects this touchscreen (ESP32-S3) to the Daisy Seed3 running NAM, over UART. This is the
Phase 0 plan from the session of 2026-10-01; decisions marked **(approved)** were confirmed by
the user. The protocol details live in `SEED_LINK_PROTOCOL.md` once written (Phase 2).

## 1. Baseline (unchanged firmware, measured on hardware)

Firmware `fbd5d99`, ELF `#d3b6e223`, ESP-IDF v5.5.5, DOUBLE_DIRECT, fresh `sdkconfig` from
`sdkconfig.defaults`. Captured with `tools/capture_log.py` on COM7 (CH343, SW1 = UART1). Raw logs
are in the git-ignored `logs/` folder. Values are min / avg / max over the 1 s `DIAG` lines;
frame times only count seconds in which something was redrawn.

| Scenario (DIAG lines) | frame_avg ms | frame_max ms | tap ms | redraws/s | cpu0 % | cpu1 % | heap_min KB |
|---|---|---|---|---|---|---|---|
| Idle after boot (71) | - | - | - | 0 / 0 / 1 | 5.3 / 5.4 / 10.2 | 0.3 / 0.9 / 36.3 | 58.7 |
| Parameter slider drag (29) | 33.6 / 35.3 / 36.4 | 36.2 / 39.4 / 49.6 | - | 4 / 19.3 / 27 | 7.3 / 16.8 / 22.3 | 4.2 / 13.0 / 18.6 | 58.7 |
| Master slider drag (28) | 30.5 / 34.1 / 35.8 | 36.2 / 36.6 / 37.6 | - | 11 / 21.1 / 28 | 8.1 / 10.7 / 12.5 | 4.8 / 7.4 / 9.1 | 58.7 |
| Chip taps, about 1/s (28) | 33.6 / 68.0 / 272.7 | 37.4 / 70.3 / 272.7 | 89.1 / 164.6 / 393.1 | 0 / 2.8 / 18 | 5.4 / 10.7 / 24.1 | 0.3 / 7.7 / 26.2 | 58.7 |

Other readings: free internal heap 96.9 KB, largest internal block 51.0 KB, free PSRAM
5019 KB of 7467 KB, panel 26.2 Hz, no RGB restarts, no expander recoveries, `exio` = 0xFF.

Findings from the baseline:
- **Chip taps are much slower than the drags.** Tapping a chip calls `rebuild_panel()`, which
  deletes and recreates the model cards and knobs; one such frame took 273 ms, and the diag
  watchdog's 300 ms lock probe failed once ("LVGL task unresponsive for 1s", recovered the next
  second, no RGB restart). HANDOFF.md's 13-60 ms tap figure predates the feature-parity port.
  Not in scope for the link, but the link work must not make it worse, and it is a good
  candidate for a later fix (restyle instead of rebuild when the effect has the same layout).
- The `exio` byte is 0xFF, which confirms EXIO5 (USB_SEL) is high, see section 4.
- The ESP32 ROM and 2nd-stage bootloader print about 4 KB of text on UART0 at every reset,
  before the app starts. Once UART0 is the Seed link, the Seed receives this; COBS framing and
  the CRC make the Seed drop it (section 5).

## 2. Hardware facts (verified against schematics)

**Waveshare ESP32-S3-Touch-LCD-7B** (schematic PDF from the Waveshare wiki):
- SW1 drives an FSUSB42 mux (U13) that connects UART0 (GPIO43 TX / GPIO44 RX) either to the
  CH343 on Type-C "UART1" (silkscreen; the "USB TO UART" port) or to header **H3 "UART2"**:
  pin 1 3V3, pin 2 GND, pin 3 RX (to ESP GPIO44), pin 4 TX (from ESP GPIO43).
- Native USB (GPIO19/20) goes through a second FSUSB42 (U9) to Type-C "USB" or to the CAN
  transceiver. Its select line USB_SEL is **EXIO5**, with a 10K pull-down: low = USB, high = CAN.
- The only GPIO not used by the board is **GPIO6**, on the 3-pin "GPIO" header J8 (3V3, GND, GP6).
  GPIO15/16 go to the RS485 transceiver, GPIO19/20 to USB/CAN. There is no free pin pair for
  a second UART.
- Header H1 "I2C" (3V3, GND, SDA, SCL) is the same GPIO8/9 bus as the GT911 touch and the IO
  expander (0x24).

**Seed3 carrier** (`seed3-carrier-board`, traced through the PCB pad nets):
- J4 "DISPLAY_UART", 1x4 2.54 mm: **pin 1 GND, pin 2 Seed TX (USART1_TX, D13/PB6), pin 3 Seed
  RX (USART1_RX, D14/PB7), pin 4 +5V**. Plain 3.3 V logic, no series resistors.
- The carrier also has two analog pots on the Seed ADC (VOLUME on A0, TONE on A1), which the
  Seed firmware doesn't read yet.

### Cable (ESP32 H3 to Seed carrier J4)

| ESP32 H3 | Seed carrier J4 |
|---|---|
| pin 2 GND | pin 1 GND |
| pin 3 RX | pin 2 Seed TX |
| pin 4 TX | pin 3 Seed RX |
| pin 1 3V3 | **not connected** |
| **not connected** | pin 4 +5V |

**Warning: never use a straight 4-wire cable.** J4 pin 4 is +5 V and H3 pin 1 is the ESP32's
3.3 V rail. A straight cable connects them.

## 3. Design

All new code lives in `firmware/main/` (subfolders), not new IDF components, except that
`seed_link_proto/` is self-contained so the host tests and the Seed owner can compile it alone.

| Module | Job |
|---|---|
| `rig/rig_state.c/.h` | Single source of truth for audio state: chain, on/off, model, knobs, master, mute. Every mutation goes through it; it compares old and new, marks dirty bits and notifies the link task. Non-UI sources (knobs, presets, SD) call the same API; the UI follows under `board_lvgl_lock()` with a guard so widget updates don't call back into rig_state. |
| `seed_link_proto/` | Pure C99, no malloc, no ESP-IDF: COBS, CRC-16/CCITT-FALSE, CRC32, message structs, encode/decode, a bounded stream decoder. Drops into the Seed's C++20 build unchanged. |
| `seed_link.c` | Real implementation behind the existing `seed_link.h`: a FreeRTOS task owning the UART (Kconfig port/pins/baud, default UART0, 43/44, 1 Mbaud, `CONFIG_UART_ISR_IN_IRAM`), handshake, heartbeat, ping, coalescing, snapshots, model upload, telemetry into `seed_link_stats_t`. `seed_link_ping()` only raises a flag, so it never blocks LVGL. |
| `param_map` | `tools/param_ids.json` (append-only registry: key to id) and `tools/gen_params.py` generating `param_map_gen.h` plus the markdown table. The build runs `gen_params.py --check` and fails if the registry and `ui_effects_data.c` disagree. |
| `sd/sd_storage.c` | microSD over SDSPI (GPIO11 MOSI, 12 SCK, 13 MISO) at `/sdcard`, CS on EXIO4. |
| `nam/nam_loader.c` | Scan, parse, validate, pack and cache `.nam` files (section 7). |
| `knobs/knobs.c` | Two Adafruit STEMMA QT rotary encoders (section 8). |

Threading: the LVGL task (core 1) only records changes; the link task (core 0, priority below
LVGL), the SD/model worker and the knob task do all I/O. The IO expander driver gets a mutex,
because the diag task, the SD chip select and later code all write it.

## 4. Console move **(approved)**

| Mode | SW1 | Seed link | Console + DIAG + flashing |
|---|---|---|---|
| With the Seed (lab) | UART2 | H3 header to Seed J4 | Type-C "USB" (native USB Serial/JTAG) |
| With the mock (now) | UART1 | Type-C "USB TO UART" (CH343, COM7), `tools/mock_seed.py` | Type-C "USB" (native USB Serial/JTAG) |

- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults`.
- `io_expander_init()` drives EXIO5 low. Today it writes 0xFF to every output, so EXIO5 is
  high and the native USB port is routed to the CAN transceiver: it can't enumerate. The
  expander keeps that state across ESP32 resets, so the first flash after the change goes
  through COM7 (CH343).
- The preview's Web Serial Test Mode keeps working: USB Serial/JTAG is a CDC-ACM port.
- Risk to measure: with no host on the native port, console writes can wait for a timeout;
  check that the 1 s DIAG print doesn't stall.

## 5. Protocol v1

As specified in `SEED_LINK_PROMPT.md` section 7 (COBS, 0x00 delimiters, header
`ver type seq flags len`, CRC-16/CCITT-FALSE, little-endian, max payload 256 bytes), with:
- `flags` bit 0 = ack requested (only `MODEL_*` set it); all state messages are idempotent.
- `SNAPSHOT_BEGIN/END` carry a `snapshot_id`, so the Seed applies a snapshot as one unit.
- `SET_MASTER` with mute bypasses the 10 ms coalescing and is sent immediately.
- `HELLO` carries a `peer_kind` byte (Seed or mock), so the ESP32 can label mock data **MOCK**.
- The Seed ignores undecodable bytes (the ESP32 boot banner, section 1).
- Link loss: the Seed keeps its last state, never jumps to defaults.

## 6. Amp models **(direction approved)**

The idea is a set of presets built on the Seed's real NAM amps, with more amp profiles imported
from TONE3000 through the SD card.
- The AMP picker shows the Seed built-ins in AmpId order: **1 Fender Twin65, 2 Vox AC30
  Chimey, 3 Marshall JCM800 G5**, then the SD profiles. Names and loaded state come from the
  Seed's HELLO/STATUS when connected; with no link the cards say the Seed isn't connected
  (no fake "loaded").
- "Marshall Plexi" becomes "Marshall JCM800 G5" and "Mesa Rectifier" is removed. The presets
  keep their names and character: Crunch Rock, Lead Boost and Metal Tight use JCM800 G5.
- A preset or saved rig can reference an SD profile by hash; if the card doesn't have it, the
  UI says so.
- Only TONE3000 **A2** downloads run on the Seed (its A2-Lite engine); anything else is
  rejected with the reason.
- The AMP knobs (bass, mid, treble, level) and all other effects are sent as parameters and
  documented as "not yet implemented on the Seed".
- `preview/index.html` is updated to match.

## 7. SD model pipeline

TONE3000 A2 file on `/sdcard/nam/`, read into PSRAM (size capped), tokenized with a zero-alloc
JSON tokenizer (a cJSON tree of a several-hundred-KB container could take megabytes), Lite
submodel extracted (`max_value == 0.5`), validated exactly like `convert_a2.py`, weights parsed
with `strtod` then rounded to float, packed `[out][in][tap]` to `[tap][in][out]`, CRC32, cached
as `/sdcard/nam/.cache/<sha256>.a2l`. Golden tests compare against `convert_a2.py` byte for byte.

## 8. Physical knobs

2x Adafruit STEMMA QT rotary encoder (seesaw ATSAMD09) on header H1 (the shared touch bus,
since no other pins are free). Knob 1 at 0x36: master volume, push = mute. Knob 2 at 0x37 (A0
address jumper): the parameter open in the slider, else the selected effect's first knob;
push = bypass the selected effect. Both INT pins to GPIO6, so the bus is only read when a knob
moves. Touch latency must stay within 10 % of the baseline.

## 9. Draft parameter IDs

`id = (effect wire id << 8) | knob index`. Effect wire ids: gate 1, comp 2, eq 3, drive 4,
amp 5, octave 6, chorus 7, phaser 8, tremolo 9, delay 10, reverb 11, cab 12. Plus 0x0001
`master.volume` (0-100) and 0x0002 `master.mute` (0/1). All knobs are 0-100 in UI units. Once
frozen in `tools/param_ids.json`, IDs are never renumbered; a removed knob's id is retired.

| Effect (id range) | Knob keys in id order, starting at xx = 00 |
|---|---|
| gate 0x01xx | gate.thresh, gate.attack, gate.release, gate.range |
| comp 0x02xx | comp.thresh, comp.ratio, comp.attack, comp.release, comp.gain |
| eq 0x03xx | eq.low, eq.mid, eq.high, eq.level |
| drive 0x04xx | drive.gain, drive.tone, drive.level |
| amp 0x05xx | amp.gain, amp.bass, amp.mid, amp.treble, amp.level |
| octave 0x06xx | octave.mix, octave.octUp, octave.octDown |
| chorus 0x07xx | chorus.rate, chorus.depth, chorus.mix |
| phaser 0x08xx | phaser.rate, phaser.depth, phaser.mix |
| tremolo 0x09xx | tremolo.rate, tremolo.depth |
| delay 0x0Axx | delay.time, delay.feedback, delay.tone, delay.mix |
| reverb 0x0Bxx | reverb.decay, reverb.predelay, reverb.tone, reverb.mix |
| cab 0x0Cxx | cab.lowcut, cab.highcut, cab.level |

43 knobs. The generated table in `SEED_LINK_PROTOCOL.md` adds each knob's meaning and the
"implemented on the Seed" column.

## 10. Borrowed from Eric-Touchscreen-UI

Nothing in code. It has no SD, UART or knob code. Its `amp_profile` component (linker-wrapped
LVGL timing) is a good idea for later profiling, but it has no license header, so ask Eric
before copying it. Its Waveshare `io_extension.h` confirmed EXIO5 (0 = USB, 1 = CAN).

## 11. Risks, and where the prompt was wrong

- **EXIO5 bug** (section 4): the native USB port has been dead since the first firmware.
- **The knobs can't get their own bus.** HANDOFF.md suggests another bus, but the board has
  no free pins for one. INT-driven reads keep the bus quiet.
- **The expander driver isn't thread-safe** (`s_out` read-modify-write from several tasks).
  Add a mutex before SD or knobs use it. The SD card's CS is held low while mounted, so the
  diag watchdog's re-assert writes the same level and can't glitch it.
- **The tap path is already slow** (section 1): 165 ms average, 393 ms worst. Link work must
  not add LVGL work per change.
- **JSON size**: a TONE3000 A2 container holds all its submodels; a DOM parser could take
  megabytes of PSRAM. A zero-alloc tokenizer avoids that.
- **Validation semantics**: Python compares with `==`, so `1 == 1.0`; the C port matches that.
- **No host C compiler on PATH**; VS 2022 Build Tools is installed and will be used.
- **App partition**: the default table gives the app 1 MB (26 % free now) of the 16 MB flash.
  A custom partition table will likely be needed as features land.
- The prompt says H3/J4 are both "3.3 V": the signals are, but **J4 pin 4 is +5 V** (section 2).
