# Handoff — read this first

Touchscreen UI for a guitar multi-effects / practice amp (CU Boulder capstone, team B1narySolutions,
sponsor-driven). This repo is the **UI board** only. The audio DSP runs on a separate Electrosmith
Daisy Seed ("SEED3") running Neural Amp Modeler — that code lives in the team repo
`B1narySolutions/realtime-nam-seed3` (teammates' work: read it, don't modify it without asking).

## Hardware

- **Waveshare ESP32-S3-Touch-LCD-7B** — 7" 1024x600 RGB IPS, GT911 touch. The ESP32-S3 (8 MB PSRAM)
  is built into the board; no separate ESP32 needed.
- Board config is copied from Waveshare's official ESP-IDF example, not guessed:
  github.com/waveshareteam/ESP32-S3-Touch-LCD-7B → `examples/ESP-IDF/17_lvgl_v9_demo`.
  - RGB: pclk 24 MHz, HPW 162 / HBP 152 / HFP 48, VPW 45 / VBP 13 / VFP 3, pclk_active_neg.
  - I2C bus GPIO8 (SDA) / GPIO9 (SCL), shared by GT911 touch and the IO expander.
  - IO expander at I2C 0x24 (register-based, **not** a CH422G like the 800x480 board):
    EXIO1 touch reset, EXIO2 backlight enable, EXIO3 LCD reset; reg 0x05 = backlight PWM
    (inverted), reg 0x06 = ADC → battery volts = raw * 3 * 3.3 / 1023.
- Flash/console port: the Type-C labelled **USB TO UART** (CH343 USB-serial chip).

## Status (2026-10-01)

- `firmware/` builds with **ESP-IDF v5.5.5** (`idf.py build`; the only warnings are LVGL 9.6
  deprecations of `lv_obj_add_flag`/`lv_obj_remove_flag`). **Flashed and running on the 7B**:
  display, GT911 touch, IO expander, backlight and the `DIAG` serial line all work. The user
  reported colours and touch as good.
- Fixed in the first hardware round: dark/low-contrast text (LVGL default theme was in light
  mode and the screen had no text colour - now dark theme + contrast-checked tokens in
  `ui_theme.h`), backlight default 80 % -> 100 %, the parameter slider popup scrolling (content
  overflowed its card), colour bars not closing, and the lag described below.
- Reworked Test Mode (build ID, per-core CPU, frame timing, touch-alignment ring) and the 16 MB
  flash setting run on hardware (boot log + DIAG line checked).
- **Feature parity with the preview** (ported 2026-10-01, boots clean; UI not yet walked
  through on hardware): presets drawer (5 presets), model cards per effect, 11 effects + pinned
  CAB with its frequency-response curve, add/remove catalog, scrolling chain (hold a chip to
  reorder), IN/OUT meters (real SEED3 peaks only - empty with no link), master % readout.
  SAVE now persists the rig to NVS and settings persist on closing Settings; both reload at boot.
- Palette lifted (all surfaces/greys brighter; every colour now a token in `ui_theme.h`) after
  the user found the near-black theme hard to read even at full backlight. The browser preview
  still uses the old darker palette.
- `preview/index.html` is the standalone browser prototype (hosted on Netlify by the user).
  `ui_effects_data.c` mirrors its EFFECTS/MODEL_*/PRESETS tables - keep the two in sync.

## Display performance (measured 2026-10-01)

Every UI frame is timed around LVGL's display events (`diag.c`); results are in the `ui` object
of the `DIAG` line and in Test Mode.

| Setting | Frame time (small change) | Tap to frame | Notes |
|---|---|---|---|
| `TRIPLE_PARTIAL` (adapter + Waveshare default) | ~75 ms | ~80 ms | ~51 ms of it is a CPU memcpy of the whole un-redrawn screen (~1.2 MB PSRAM to PSRAM) every frame - the S3 has no DMA2D |
| `DOUBLE_DIRECT` (now) | ~27-50 ms | ~36 ms median | LVGL draws straight into the back frame buffer; only dirty areas are synced. Most of the "flush" time is now the VSYNC wait |

Also: LVGL task pinned to core 1 (core 0 takes the RGB bounce-buffer ISR); internal RAM free
went 72 KB -> 175 KB. Remaining ceilings and leads:

- **Panel refresh is only ~26 Hz**: 24 MHz pclk / (1386 x 661 incl. porches), Waveshare's timings.
  Raising pclk (e.g. 30 MHz) is the next lever; watch for the image shifting (PSRAM bandwidth) and
  consider `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` if trying it.
- **~23 ms of fixed per-frame overhead** outside the flush, even for tiny redraws - not yet
  profiled. Suspects: per-area render setup with 2 SW draw threads, layout passes, objects in PSRAM.
- SIMD blending is off (`LV_DRAW_SW_ASM_NONE`). Espressif's S3 SIMD routines (esp_lvgl_port) only
  build against LVGL 9.1, so using them on 9.6 means porting the assembly hooks.
- `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` (hot LVGL routines in IRAM) is untried; costs internal RAM.

## Firmware layout

```
firmware/main/
  app_main.c          board_init -> diag_init -> ui_main_init -> diag serial stream
  board_init.c/.h     RGB panel, GT911, IO expander, esp_lvgl_adapter wiring, backlight, battery
  io_expander.c/.h    0x24 expander driver (outputs, PWM backlight, ADC)
  diag.c/.h           live diagnostics snapshot + "DIAG {json}" line on serial every second
  seed_link.c/.h      seam for the ESP32 <-> SEED3 link (transport TBD); reports "not connected"
  ui/ui_theme.h       every colour (contrast-checked tokens) and shared sizes
  ui/ui_effects_data.c  effects, models, default values, presets, IR curve shapes (mirrors preview)
  ui/ui_main.c        screen layout, app state, presets drawer, catalog, settings, NVS save/load
  ui/ui_effect_chip.c tap = select, tap again = bypass toggle; hold + drag to reorder chain
  ui/ui_ir_curve.c    CAB frequency-response graph (illustrative, see ui_effects_data.h)
  ui/ui_knob.c        display-only arc; tap opens the shared vertical slider
  ui/ui_vertical_slider.c  the one slider used to edit every parameter
  ui/ui_test_mode.c   on-device diagnostics screen, touch test, colour bars
```

LVGL 9.6 + esp_lvgl_adapter + esp_lcd_touch_gt911 come from the component registry (see
`main/idf_component.yml`; `dependencies.lock` pins exact versions).

## Hard rules (from the sponsor)

- Parameters are **never** edited by dragging a rotary knob. Tapping a knob opens the vertical
  slider; tap outside to dismiss.
- Effect chips: first tap opens that effect's panel, next tap bypasses, next tap re-enables.
- Effects chain is reorderable; CAB (impulse response) stays pinned at the end of the chain.
- **No fake/simulated data** anywhere in Test Mode. Every value must be a real reading or an
  honest "not connected / —".

## Next steps

1. Walk through the ported UI on hardware (presets, model cards, catalog, hold-to-reorder,
   SAVE + reboot, Settings persistence) and fix what turns up.
2. Optionally bring the preview's palette up to the lifted `ui_theme.h` values.
3. Design the SEED3 link. As of 2026-10-01 the team is leaning towards **UART** for the SEED3
   (I2C is wanted for the potentiometers) - confirm, pick free GPIOs, and coordinate the message
   format with the realtime-nam-seed3 repo. Only `seed_link.c` should need a real transport; Test
   Mode already reads through it. If the pots go on I2C, prefer a bus other than GPIO8/9: that one
   carries the GT911, and polling extra devices on it delays touch reads.
4. Verify the new Test Mode on hardware, then continue on the performance leads above.

## Working conventions

- The user (jasm4916) runs all git commits/pushes. Make file edits, then give them the exact
  `git add <files>` / `git commit` / `git push` commands.
- Verify on real output (build it, flash it, read the serial log / look at the screen) rather
  than assuming code works.
- `firmware/sdkconfig` is gitignored and an existing one ignores later `sdkconfig.defaults`
  changes. After pulling a defaults change, delete `firmware/sdkconfig` and rebuild (Test Mode
  shows "run-time stats off" for CPU load until you do).
- Test Mode's Firmware row shows the git version plus `#` and the first 8 hex chars of the ELF
  SHA256, so you can tell exactly which build is on a board when several people flash it.
