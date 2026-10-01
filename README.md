# Multi-FX Console UI — Waveshare ESP32-S3 7" (1024x600)

One-page touchscreen UI for a multi-effects unit: a reorderable effects chain,
a per-effect knob panel, and a single shared vertical slider that's the only
way any value ever gets adjusted (no dragging knobs directly — that was the
sponsor's #1 requirement).

An interactive browser prototype of the design lives in `preview/index.html`
(a single self-contained file — open it in a browser, or drag the `preview`
folder onto Netlify to host it). Try: tap a knob to open the slider, tap a
chip once to select it and again to toggle bypass, and drag a chip to reorder
the chain.

**New here (or a new Claude session)? Read `docs/HANDOFF.md` first.**

## The two interactions the sponsor asked for

1. **The multipurpose slider.** Tapping any knob never adjusts it directly —
   it opens one shared vertical slider, pre-loaded with that knob's label,
   color, and current value. Drag it, tap its track, or use the +/- buttons;
   tap anywhere outside the card (or the × button) to dismiss it. One
   component, reused for every knob on every effect.
2. **Tap-to-cycle effects, no separate bypass buttons.** Tapping a chip that
   isn't open just selects it and opens its panel. Tapping the *already
   open* chip toggles it between on and bypassed. Drag a chip (past a small
   move threshold, so it doesn't fight with a tap) to reorder the chain —
   the effect keeps its on/bypass state wherever it lands.

## What's in this folder

```
lvgl_pro/          LVGL Editor (Pro/XML) project — open this folder in the editor
  project.xml        target + 1024x600 display config
  globals.xml        palette, per-effect colors, shared fonts/subjects
  components/        effect_chip, knob_ring, vertical_slider, top_bar, bottom_bar
  screens/           main_screen.xml — assembles all of the above

firmware/          ESP-IDF component — the actual working implementation
  main/
    app_main.c         entry point
    board_init.c/.h    display + touch bring-up (⚠ see VERIFY notes inside)
    ui/
      ui_main.c/.h            builds the screen, owns app-level state
      ui_effects_data.c/.h    the 7 effects, their knobs, colors, model options
      ui_effect_chip.c/.h     chip widget: tap-to-cycle + drag-to-reorder
      ui_knob.c/.h            the read-only arc "knob" that opens the slider
      ui_vertical_slider.c/.h the shared slider popup
      ui_theme.h              color palette shared with the XML side

docs/
  HANDOFF.md         current state, what's stubbed, what to do next
```

## Why two implementations of the same screen

LVGL Editor's XML format is great for laying out and restyling widgets
visually, but the actual behavior you asked for — a tap that means
different things depending on what's already selected, live drag-reordering,
one popup that retargets itself to whatever was tapped — is exactly the kind
of thing LVGL Pro's own docs say to write as plain C event callbacks on top
of the exported structure, not in XML. So:

- **`lvgl_pro/`** is what you'd open in the LVGL Editor to tweak colors,
  spacing, and fonts visually, and to export fresh C for the *static*
  structure. It's commented wherever a widget's real behavior lives in C
  instead.
- **`firmware/main/ui/`** is hand-written LVGL v9 C that already implements
  all of the interaction logic end to end. It's the one to actually build
  and flash; treat the XML as the visual-editing front end for it, not as a
  separate parallel version.

I derived the XML tag names and `project.xml`/`globals.xml` schema from
LVGL's published docs rather than from hands-on testing in the actual
Editor app (I don't have it installed in this environment) — if a tag name
doesn't match what your installed version expects, the fix is almost
certainly a rename, not a redesign. The hand-written C in `firmware/` has no
such dependency and is what I'd trust first.

## Building

From an **ESP-IDF 5.5 PowerShell** (Start menu), or a normal PowerShell after
running `C:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1`:

```
cd C:\Users\QwexM\Desktop\projects\amp-multifx-ui\firmware
idf.py build
idf.py -p COM5 flash monitor
```

Use the board's **USB TO UART** Type-C port, and replace `COM5` with whatever
Device Manager shows under "Ports (COM & LPT)". Exit the monitor with Ctrl+].

Board config (`board_init.c`, `io_expander.c`, `sdkconfig.defaults`) matches
Waveshare's official ESP-IDF example for the ESP32-S3-Touch-LCD-7B
(github.com/waveshareteam/ESP32-S3-Touch-LCD-7B, `17_lvgl_v9_demo`): pins, RGB
timings, GT911 reset sequence, IO expander at 0x24 (backlight PWM + battery ADC).

## Test Mode

Menu → Test Mode on the device shows live readings (firmware build ID, per-core CPU load,
heap, PSRAM, chip temperature, battery voltage and its 10 s range, backlight, panel refresh
rate, UI frame time and tap-to-frame latency, a touch test with an alignment ring, colour
bars) and the SEED3 link status. The firmware also prints one `DIAG {json}` line per second on the serial
port; the preview's Test Mode reads those over Web Serial (Chrome/Edge) and shows the
same live values. Nothing in either is simulated. SEED3 fields read "Not connected"
until `seed_link.c` gets a real transport.
