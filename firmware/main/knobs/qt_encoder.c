#include "qt_encoder.h"

uint32_t qt_encoder_u32(const uint8_t b[4]) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
int32_t qt_encoder_detents(const uint8_t bytes[4], bool reverse) {
    uint32_t raw = qt_encoder_u32(bytes);
    int64_t value = raw >= 0x80000000u ? (int64_t)raw - 0x100000000LL : (int64_t)raw;
    value = -value;
    if (reverse)
        value = -value;
    /* One full control sweep is enough even after a very large accumulated delta. */
    if (value > 100)
        value = 100;
    if (value < -100)
        value = -100;
    return (int32_t)value;
}
bool qt_encoder_button_update(qt_encoder_button_t *b, bool pressed, uint32_t now) {
    if (!b->initialized) {
        b->initialized = true;
        b->raw_pressed = b->stable_pressed = pressed;
        b->changed_ms = now;
        return false;
    }
    if (pressed != b->raw_pressed) {
        b->raw_pressed = pressed;
        b->changed_ms = now;
    }
    if (pressed == b->stable_pressed || (uint32_t)(now - b->changed_ms) < 20)
        return false;
    b->stable_pressed = pressed;
    return pressed;
}
