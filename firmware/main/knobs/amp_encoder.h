#pragma once
#include <stdbool.h>
#include <stdint.h>

/* UI task: publish a popup focus change and consume events from the UI timer. */
void amp_encoder_set_popup(bool open);
void amp_encoder_process(void);
/* Hardware worker: enqueue only, never access LVGL. Encoder 0=master, 1=popup. */
bool amp_encoder_turn(unsigned encoder, int32_t detents);
bool amp_encoder_press(unsigned encoder);
