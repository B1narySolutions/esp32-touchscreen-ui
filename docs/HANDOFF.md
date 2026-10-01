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

- `firmware/` builds clean with **ESP-IDF v5.5.5** (`idf.py build`). **Not yet run on real hardware**
  — the first flash is the next step. Expect a bring-up round: check colours, touch alignment
  (GT911 `swap_xy`/`mirror_*` flags in `board_init.c`), and Test Mode readings.
- `preview/index.html` is a standalone browser prototype of the UI (hosted on Netlify by the user).
  It is **ahead of the firmware**: it has presets drawer, model cards per effect, pinned CAB/IR
  block, add/remove effects catalog. The firmware UI still has the older 7-effect layout and needs
  a feature-parity port once hardware bring-up is confirmed.

## Firmware layout

```
firmware/main/
  app_main.c          board_init -> diag_init -> ui_main_init -> diag serial stream
  board_init.c/.h     RGB panel, GT911, IO expander, esp_lvgl_adapter wiring, backlight, battery
  io_expander.c/.h    0x24 expander driver (outputs, PWM backlight, ADC)
  diag.c/.h           live diagnostics snapshot + "DIAG {json}" line on serial every second
  seed_link.c/.h      seam for the ESP32 <-> SEED3 link (planned over I2C); reports "not connected"
  ui/ui_main.c        screen layout, app state, side menu (Menu -> Test Mode)
  ui/ui_effect_chip.c tap = select, tap again = bypass toggle; drag to reorder chain
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

1. First flash on the 7B; verify display, touch, Test Mode, and the `DIAG` serial line.
2. Port the preview's newer UI (presets, model cards, CAB, catalog) into the firmware.
3. Design the I2C link with the SEED3 (ESP32 master, Daisy slave at its own address on the shared
   bus, or a separate bus) — coordinate the message format with the realtime-nam-seed3 repo.
   Only `seed_link.c` should need a real transport; Test Mode already reads through it.

## Working conventions

- The user (jasm4916) runs all git commits/pushes. Make file edits, then give them the exact
  `git add <files>` / `git commit` / `git push` commands.
- Verify on real output (build it, flash it, read the serial log / look at the screen) rather
  than assuming code works.
