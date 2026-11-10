#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Adafruit 5880 uses the opposite raw direction to clockwise-positive. */
int32_t qt_encoder_detents(const uint8_t bytes[4], bool reverse);
uint32_t qt_encoder_u32(const uint8_t bytes[4]);

typedef struct {
    bool initialized, raw_pressed, stable_pressed;
    uint32_t changed_ms;
} qt_encoder_button_t;

/* One event after a stable 20 ms press. Holding/bounce/startup-held do not repeat. */
bool qt_encoder_button_update(qt_encoder_button_t *button, bool pressed, uint32_t now_ms);
