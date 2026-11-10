#include "amp_encoder.h"
#include "freertos/FreeRTOS.h"
#include "rig_state.h"
#include "ui_vertical_slider.h"

#define INPUT_CAPACITY 64
typedef struct {
    uint32_t focus;
    int16_t delta;
    uint8_t encoder;
    bool press;
} input_t;
static input_t inputs[INPUT_CAPACITY];
static unsigned first, count;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t focus_token;

void amp_encoder_set_popup(bool open) {
    uint32_t previous = __atomic_load_n(&focus_token, __ATOMIC_RELAXED);
    // The low bit marks an open popup. Each show/hide starts a new focus session.
    uint32_t next = ((previous & ~1u) + 2u) | (open ? 1u : 0u);
    __atomic_store_n(&focus_token, next, __ATOMIC_RELAXED);
}
static bool enqueue(input_t event) {
    bool accepted = false;
    portENTER_CRITICAL(&input_lock);
    if (count < INPUT_CAPACITY) {
        inputs[(first + count) % INPUT_CAPACITY] = event;
        count++;
        accepted = true;
    }
    portEXIT_CRITICAL(&input_lock);
    return accepted;
}
bool amp_encoder_turn(unsigned encoder, int32_t delta) {
    if (encoder > 1)
        return false;
    if (!delta)
        return true;
    if (delta > 100)
        delta = 100;
    if (delta < -100)
        delta = -100;
    uint32_t token = encoder == 1 ? __atomic_load_n(&focus_token, __ATOMIC_RELAXED) : 0;
    if (encoder == 1 && !(token & 1u))
        return true;
    input_t event = {.focus = token, .delta = (int16_t)delta, .encoder = (uint8_t)encoder};
    return enqueue(event);
}
bool amp_encoder_press(unsigned encoder) {
    if (encoder > 1)
        return false;
    if (encoder == 1)
        return true; /* Only the master button has an action. */
    input_t event = {.encoder = 0, .press = true};
    return enqueue(event);
}
void amp_encoder_process(void) {
    for (unsigned handled = 0; handled < INPUT_CAPACITY; handled++) {
        input_t event;
        bool found = false;
        portENTER_CRITICAL(&input_lock);
        if (count) {
            event = inputs[first];
            first = (first + 1) % INPUT_CAPACITY;
            count--;
            found = true;
        }
        portEXIT_CRITICAL(&input_lock);
        if (!found)
            break;
        if (event.encoder == 0) {
            if (event.press)
                rig_set_muted(!rig()->muted, RIG_SRC_KNOB);
            else
                rig_set_master_volume(rig()->master_volume + event.delta, RIG_SRC_KNOB);
        } else if (event.focus == __atomic_load_n(&focus_token, __ATOMIC_RELAXED)) {
            uint8_t fx, knob;
            if (ui_vertical_slider_target(&fx, &knob))
                rig_set_knob(fx, knob, g_knob_values[fx][knob] + event.delta, RIG_SRC_KNOB);
        }
    }
}
