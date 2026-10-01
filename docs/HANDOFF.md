# Handoff notes

## Done
- Full 1024x600 screen: top bar, 7-slot effects rail, context-sensitive knob
  panel, bottom bar — matches the browser prototype at
  https://claude.ai/artifact/6wB1xEdoNvxQipXzX5eYNE
- Tap-to-cycle chip state machine (select → bypass → on → bypass → …)
- Drag-to-reorder on the chips, live-swapping as you drag past a neighbor
- The shared vertical slider popup: opens from any knob, drag/tap-track/±
  buttons all work, backdrop-tap and × both dismiss it
- Settings overlay with a backlight slider and Wi-Fi/BLE toggle switches
  (UI only — see TODOs below)
- Master volume slider + mute, Presets button cycles a demo preset list,
  Save clears the "EDITED" badge

## Stubbed — needs real hookups, marked `TODO` in the code
- **Audio engine**: every place a knob/chip/master value changes has a
  `// TODO` where it should push into the real DSP chain
  (`ui_main_on_chip_tapped`, `ui_main_on_chip_reordered`,
  `ui_vertical_slider.c:apply_value`, `master_slider_cb`, `mute_btn_cb`)
- **Persistence**: `save_btn_cb` and `presets_btn_cb` in `ui_main.c` don't
  read/write flash yet — plug in NVS (or your preset format) there
- **Wi-Fi / BLE**: the toggle switches in the Settings overlay are cosmetic;
  `wifi_switch_cb` / `ble_switch_cb` need the real radio calls
- **Backlight**: `board_set_backlight_pct()` in `board_init.c` just logs —
  needs the real PWM/expander call

## Needs hardware verification before flashing
See the header comment in `firmware/main/board_init.c`. Specifically:
- The 16 R/G/B data-bus GPIOs (only the 4 RGB control signals + touch I2C
  pins were confirmed from the public Waveshare wiki)
- RGB panel timing (pclk, porches) for the 1024x600 ST7701 panel
- Which chip drives TP_RST / backlight enable on this board's IO expander
  (likely a CH422G based on Waveshare's other ESP32-S3 boards, but confirm
  for the 7B specifically) and its driver library/API

Pull these from Waveshare's own ESP-IDF example for the 7B board
(linked from https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B) rather
than guessing further — wrong RGB timings just show a blank/garbled panel,
so it's a safe thing to iterate on once real hardware is on the bench.

## Suggested next pass
1. Fill in `board_init.c`, get a blank screen showing, confirm touch
   coordinates land where expected (GT911 orientation/mirroring flags are
   easy to get backwards — the `flags` in `esp_lcd_touch_config_t` are
   there for exactly that).
2. Wire one real DSP parameter (e.g. AMP gain) end to end through the slider
   as a smoke test before doing the rest.
3. Open `lvgl_pro/` in the Editor to start dialing in exact spacing/fonts
   against the real panel — colors and layout constants are centralized in
   `globals.xml` / `ui_theme.h` so they're one edit each.
