#pragma once
#include "lvgl.h"

// Surfaces, darkest to lightest. Lifted on 2026-10-01: the original near-black values
// (0x101013 background, 0x1b1b1f cards) crushed together on the 7B's panel even at full
// backlight. Every colour on screen should come from these tokens, so a further tweak is
// a one-file change.
#define UI_COLOR_BG        lv_color_hex(0x1b1b21) // screen, effect panel
#define UI_COLOR_INSET     lv_color_hex(0x222229) // recessed areas: graph, meters, touch pad
#define UI_COLOR_RAIL      lv_color_hex(0x1f1f25) // effects chain band
#define UI_COLOR_PANEL     lv_color_hex(0x25252c) // top/bottom bars, drawer, Test Mode panels
#define UI_COLOR_CARD      lv_color_hex(0x2c2c34) // chips, model cards, popups, buttons
#define UI_COLOR_RAISED    lv_color_hex(0x35353e) // list items on a card
#define UI_COLOR_PRESSED   lv_color_hex(0x40404a) // pressed state of cards/list items
#define UI_COLOR_DIVIDER   lv_color_hex(0x393942) // hairlines between areas and rows
#define UI_COLOR_BORDER    lv_color_hex(0x4a4a55) // outlines of chips, cards, buttons
#define UI_COLOR_TRACK     lv_color_hex(0x41414b) // slider/arc tracks, switch off
#define UI_COLOR_OFF       lv_color_hex(0x6a6872) // inactive status dots
#define UI_COLOR_BACKDROP  lv_color_hex(0x06060a) // popup dimming layer (drawn at ~60 % opacity)

// Text tiers. Contrast ratios are measured against UI_COLOR_CARD (and stay >= 5.5:1 on
// UI_COLOR_RAISED); keep text at 4.5:1 or above and icons at 3:1 or above - the panel is
// viewed dimmed and off-axis, so greys that look fine on a desktop monitor disappear on the 7B.
#define UI_COLOR_TEXT       lv_color_hex(0xf4efe6) // 12.1:1  primary text
#define UI_COLOR_TEXT_DIM   lv_color_hex(0xcdc9bf) //  8.4:1  secondary: knob captions, sublabels
#define UI_COLOR_TEXT_MUTED lv_color_hex(0xb3afa5) //  6.3:1  captions, bypassed state
#define UI_COLOR_GLYPH      lv_color_hex(0x98969f) //  4.7:1  decorative icons (drag grip)
#define UI_COLOR_ON_ACCENT  lv_color_hex(0x161409) //  9.8:1 on ACCENT: dark text on ACCENT/MUTE fills
#define UI_COLOR_ACCENT    lv_color_hex(0xe8b463)
#define UI_COLOR_OK        lv_color_hex(0x7fd9a8)
#define UI_COLOR_WARN      lv_color_hex(0xe8c463)
#define UI_COLOR_MUTE      lv_color_hex(0xe2705c)

#define UI_CHIP_W   118
#define UI_CHIP_H   82
#define UI_MAX_KNOBS 5
#define UI_EFFECT_COUNT 12  // 11 chain effects + the pinned CAB
