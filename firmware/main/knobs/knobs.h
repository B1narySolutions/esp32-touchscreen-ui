#pragma once

// Physical controls, feeding rig_state like touch does (RIG_SRC_KNOB); the UI follows through
// rig_state's observer.
//
// - At boot, logs every address that answers on the shared I2C bus (GPIO8/9).
// - Pot (CONFIG_KNOBS_GPIO6_POT): a 10k linear pot on the "GPIO" header J8 (3V3, GND, GP6),
//   read by the ADC as master volume. A stand-in until the encoders arrive.
// - Encoders (CONFIG_KNOBS_ENCODERS): Adafruit STEMMA QT rotary encoders (seesaw) on the I2C
//   header: 0x36 adjusts master volume and toggles mute on press. 0x37 adjusts the open slider.
//   Close A0 on the second board to select 0x37. Its button is unused.
//   Their INT pins can share GPIO6 (CONFIG_KNOBS_GPIO6_ENCODER_INT).
void knobs_init(void);
