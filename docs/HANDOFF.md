# Handoff: read this first

Touchscreen UI for a guitar multi-effects / practice amp (CU Boulder capstone, team B1narySolutions,
sponsor-driven). This repo is the **UI board** only. The audio DSP runs on a separate Electrosmith
Daisy Seed ("SEED3") running Neural Amp Modeler; that code lives in the team repo
`B1narySolutions/realtime-nam-seed3` (teammates' work: read it, don't modify it without asking).

**Seed link docs:** [SEED_LINK_STATUS.md](SEED_LINK_STATUS.md) (what's verified where) |
[SEED_LINK_PROTOCOL.md](SEED_LINK_PROTOCOL.md) (the wire format) |
[SEED_INTEGRATION_GUIDE.md](SEED_INTEGRATION_GUIDE.md) (for the Seed owner) |
[LAB_DAY_CHECKLIST.md](LAB_DAY_CHECKLIST.md) (bring-up with the real Seed) |
[SEED_LINK_PLAN.md](SEED_LINK_PLAN.md) (design, baseline numbers, decisions).

## Hardware

- **Waveshare ESP32-S3-Touch-LCD-7B**: 7" 1024x600 RGB IPS, GT911 touch, ESP32-S3 with 8 MB
  PSRAM and 16 MB flash, built in.
- Board config is copied from Waveshare's official ESP-IDF example, not guessed:
  github.com/waveshareteam/ESP32-S3-Touch-LCD-7B, `examples/ESP-IDF/17_lvgl_v9_demo`.
  - RGB: pclk 24 MHz, HPW 162 / HBP 152 / HFP 48, VPW 45 / VBP 13 / VFP 3, pclk_active_neg.
  - I2C bus GPIO8 (SDA) / GPIO9 (SCL), shared by GT911 touch (0x5D), the IO expander (0x24),
    and the "I2C" header H1 (physical knobs go here).
  - IO expander at I2C 0x24 (register-based, **not** a CH422G): EXIO1 touch reset, EXIO2
    backlight enable, EXIO3 LCD reset, **EXIO4 SD card CS**, **EXIO5 USB_SEL (low = native USB,
    high = CAN)**, EXIO6 LCD VDD; reg 0x05 = backlight PWM (inverted), reg 0x06 = ADC (battery
    volts = raw * 3 * 3.3 / 1023). The driver is thread-safe (mutex).
  - GPIO6 on the 3-pin "GPIO" header J8 is the only spare pin (used for the pot / encoder INT).
  - microSD: SDSPI on GPIO11 MOSI / GPIO12 SCK / GPIO13 MISO, CS on EXIO4. Card must be
    **FAT32 on an MBR partition table** (ESP-IDF's FatFs can't read GPT).
- **Two Type-C ports, and switch SW1:**
  - "USB" (native USB Serial/JTAG): console, `DIAG` line, Web Serial, flashing. Enumerates as
    "USB Serial Device (COMx)". Needs EXIO5 low, which the firmware sets at boot.
  - "USB TO UART" (CH343): with SW1 on **UART1**, carries the Seed link (UART0, GPIO43/44) to the
    PC for the simulated Seed. With SW1 on **UART2**, UART0 goes to header H3 for the real Seed.
- Seed3 carrier J4 "DISPLAY_UART": 1 GND, 2 Seed TX, 3 Seed RX, **4 +5V**. Cable it to H3 with
  three wires only (see LAB_DAY_CHECKLIST.md); a straight cable puts 5 V on the ESP32's 3.3 V rail.

## Status (2026-10-02)

Firmware builds with **ESP-IDF v5.5.5**, flashed and running on the 7B.

- UI: feature parity with the preview; presets, model cards, add/remove catalog, hold-to-reorder,
  SAVE to NVS (saved rig v2, migrates v1), Test Mode, IN/OUT meters.
- **Seed link (UART, 1 Mbaud, protocol v1)** implemented and tested against two simulated Seeds:
  `tools/mock_seed.py` and the Seed's own drop-in receiver code (`tests/host/seed_sim.c`). Not
  yet run against the real Seed (its firmware needs the receiver; see the integration guide).
- **rig_state** is the single source of truth for audio state; touch, knobs, presets and SD
  all go through it, and it logs one line per change burst.
- **AMP models**: the Seed's three built-in NAM amps (names from its HELLO) plus TONE3000 A2
  profiles from the SD card (SD Library popup). `.nam` conversion is an exact port of
  `convert_a2.py` (byte-identical); profiles upload to the Seed with CRC check and re-upload
  automatically after a Seed reboot.
- **Physical knobs**: a 10k pot on J8 works as master volume (stand-in). Driver for two Adafruit
  STEMMA QT encoders is written but untested (hardware not here yet); enable it in menuconfig.
- No fake data anywhere: anything from the simulated Seed is labelled MOCK on screen.

## Display performance (measured)

| Setting | Frame time (small change) | Tap to frame | Notes |
|---|---|---|---|
| `TRIPLE_PARTIAL` (Waveshare default) | ~50-75 ms | ~50-80 ms | ~48 ms of it is a CPU memcpy of the whole screen; pins core 0 while dragging |
| `DOUBLE_DIRECT` (**current**, LVGL task on core 1) | ~32-36 ms | chip taps 90-390 ms (panel rebuild) | **Stable with** `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`, `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` and 30-line bounce buffers. Don't undo these. |

IRAM-safe ISR gotcha: `board_init.c` raises the malloc-internal threshold just around
`esp_lv_adapter_register_display()`. If you see "user context not in internal RAM" at boot, that
guard is missing and the display will hang.

The diag task has a watchdog (`check_lvgl()`): if the LVGL lock can't be taken for 2 s it prints
the task list and restarts the RGB scan-out (`lvgl_stuck` / `rgb_restarts` in `DIAG`). **Other
tasks must not take the LVGL lock at high rates** (it starves this probe): knob and SD changes
reach the UI through rig_state's observer, which only records them for the LVGL task.

**Internal RAM is the scarce resource**: about 49 KB minimum free (58.7 KB before the Seed link;
the IRAM UART ISR costs ~5 KB). Task stacks, link state, the amp list, SD buffers and model
weights are all in PSRAM. Check `heap_min` in `DIAG` after any change.

Ceilings and leads: panel refresh ~26 Hz (pclk), ~23 ms fixed per-frame overhead, chip taps
rebuild the whole effect panel (the slowest interaction), SIMD blending off, LVGL fast-mem untried.

## Firmware layout

```
firmware/
  partitions.csv        4 MB app; NVS kept at the default offset
  sdkconfig.defaults    display stability, console on USB, UART ISR in IRAM, FatFs LFN in PSRAM
  main/
    app_main.c          board -> diag -> UI (restores the saved rig) -> seed link -> knobs -> SD
    board_init.c/.h     RGB panel, GT911, IO expander, LVGL adapter, backlight, battery, I2C bus
    io_expander.c/.h    0x24 expander driver (thread-safe)
    diag.c/.h           DIAG {json} line every second + LVGL watchdog
    seed_link.c/.h      UART transport: handshake, snapshots, coalescing, ping, telemetry, uploads
    seed_link_proto/    pure C99, shared with the Seed: framing, messages, coalescer, receiver
    rig/                rig_state (audio state), amp_models (AMP list), param_map_gen.h (generated)
    nam/                nam_a2 (.nam -> packed A2-Lite), nam_store (SD scan + cache), expected layer
    sd/                 SD card mount
    knobs/              I2C scan, pot on GPIO6, seesaw encoder driver
    ui/                 screens and widgets (ui_main.c holds the main screen, SD Library, catalog)
tools/                  capture_log, mock_seed, link_check, gen_params, golden_pack, nam_diff_test, ...
tests/host/             host unit tests (MSVC + ASan), seed_sim, nam_check, slp_examples
```

## Preview drift

`preview/index.html` mirrors `ui_effects_data.c` (effects, knobs, models, presets, including the
Seed's three built-in amps). It does **not** show things that only exist with real hardware:
- the "SD Library" card and popup, and SD profiles in the AMP row;
- the status line under the AMP cards ("Running on the Seed", upload progress, SD card state);
- the MOCK tags on the meters and in Test Mode, and Test Mode's new link rows (bus, UART
  overruns, DSP peak and overruns);
- the lifted colour palette from `ui_theme.h` (older drift, unchanged).
Its Web Serial Test Mode still parses the `DIAG` line; the new fields are additions it ignores.

## Hard rules (from the sponsor)

- Parameters are **never** edited by dragging a rotary knob. Tapping a knob opens the vertical
  slider; tap outside to dismiss.
- Effect chips: first tap opens that effect's panel, next tap bypasses, next tap re-enables.
- Effects chain is reorderable; CAB (impulse response) stays pinned at the end of the chain.
- **No fake/simulated data**: every value is a real reading or an honest "not connected / -".

## Next steps

1. Lab day with the Seed: LAB_DAY_CHECKLIST.md (the Seed owner integrates the receiver first).
2. Encoders: wire them, enable `CONFIG_KNOBS_ENCODERS`, check direction, measure tap latency.
3. Make chip taps cheaper (restyle instead of rebuilding the panel when the layout is the same).
4. Optional: preset export to SD, bringing the preview up to date (see "Preview drift" below).

If the screen stays dark after a reset done over the native USB port (seen once: the bootloader
stopped at PSRAM timing tuning), press RESET or power-cycle; see SEED_LINK_STATUS.md.

## Working conventions

- The user (jasm4916) runs commits and pushes unless they ask the assistant to commit. Never push.
- Verify on real output: build, flash, read the serial log, look at the screen.
- `firmware/sdkconfig` is gitignored; after a `sdkconfig.defaults` change delete it and rebuild.
- Hands-on tests: start `tools/capture_log.py` (and the mock) **before** asking someone to touch
  the board; the tool writes as it reads, so stop it after they're done.
- Use the IDF Python (`%USERPROFILE%\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe`)
  for the tools; it has pyserial.
- Never commit `.nam` files or anything with model weights (T3K license); `models/` is ignored.
- Test Mode's Firmware row shows the git version plus the ELF SHA256 prefix.
