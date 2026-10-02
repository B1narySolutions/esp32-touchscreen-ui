#include "knobs.h"

#include <stdio.h>
#include <string.h>
#include "board_init.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "rig_state.h"
#include "ui_main.h"
#include "ui_vertical_slider.h"

static const char *TAG = "knobs";

#define KNOB_GPIO        GPIO_NUM_6   // J8 "GPIO" header: the only spare pin on the board
#define TASK_PERIOD_MS   20           // 50 Hz
#define TASK_PRIORITY    2            // below the Seed link (5) and LVGL (6)

// ---------------------------------------------------------------------------------------------
// Boot-time I2C scan of the shared bus. Probing is a few dozen short transactions, once.

static const char *known_device(uint8_t addr) {
    switch (addr) {
    case 0x24: return "IO expander";
    case 0x5D: case 0x14: return "GT911 touch";
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D:
        return "seesaw (STEMMA QT encoder?)";
    default: return "unknown";
    }
}

static void scan_bus(i2c_master_bus_handle_t bus) {
    char line[256];
    int n = 0, found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s0x%02X %s", found ? ", " : "", a, known_device(a));
            found++;
            if (n >= (int)sizeof(line) - 1) break;
        }
    }
    ESP_LOGI(TAG, "I2C scan (GPIO8/9): %d device(s): %s", found, found ? line : "none");
}

// ---------------------------------------------------------------------------------------------
// Pot on GPIO6: master volume

#if CONFIG_KNOBS_GPIO6_POT
#define POT_SAMPLES      8
#define POT_RAW_LO       60          // dead zones at both ends so 0 % and 100 % are reachable
#define POT_RAW_HI       4035
#define POT_FIRST_MOVE   3.0f        // % of travel before the pot takes over after boot
#define POT_HYSTERESIS   1.5f        // % change needed to apply a new value (ADC noise)
#define POT_MEDIAN       5           // readings; rejects contact dropouts shorter than ~60 ms

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_ch;
static float s_pot_filtered = -1.0f, s_pot_applied = -1.0f;

static bool pot_init(void) {
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(KNOB_GPIO, &unit, &s_adc_ch) != ESP_OK) return false;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_adc_ch, &ccfg) != ESP_OK) return false;
    // With no pot connected the pin would float and its noise could move the volume. The weak
    // internal pull-down holds it at 0 instead; against a 10k pot it costs ~5 % at mid-travel.
    gpio_set_pull_mode(KNOB_GPIO, GPIO_PULLDOWN_ONLY);
    return true;
}

static void pot_poll(void) {
    int sum = 0, raw;
    for (int i = 0; i < POT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, s_adc_ch, &raw) != ESP_OK) return;
        sum += raw;
    }
    float pct = (sum / (float)POT_SAMPLES - POT_RAW_LO) * 100.0f / (POT_RAW_HI - POT_RAW_LO);
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);

    // Median of the last few readings: a wiper that briefly loses contact reads 0 (the
    // pull-down), and a median ignores that where an average would follow it.
    static float hist[POT_MEDIAN];
    static int filled, at;
    hist[at] = pct;
    at = (at + 1) % POT_MEDIAN;
    if (filled < POT_MEDIAN) filled++;
    float sorted[POT_MEDIAN];
    memcpy(sorted, hist, sizeof(float) * (size_t)filled);
    for (int i = 1; i < filled; i++) {
        for (int j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) {
            const float tmp = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = tmp;
        }
    }
    pct = sorted[filled / 2];
    s_pot_filtered = s_pot_filtered < 0 ? pct : s_pot_filtered + 0.3f * (pct - s_pot_filtered);

    // An absolute pot and an on-screen slider can disagree. The saved master volume stands at
    // boot and after screen changes; the pot takes over only once it is actually turned.
    if (s_pot_applied < 0) {
        s_pot_applied = s_pot_filtered;
        return;
    }
    static bool taken_over;
    const float moved = s_pot_filtered - s_pot_applied;
    const float threshold = taken_over ? POT_HYSTERESIS : POT_FIRST_MOVE;
    if (moved > threshold || moved < -threshold) {
        taken_over = true;
        s_pot_applied = s_pot_filtered;
        // Same rule as dragging the on-screen master slider: moving the volume un-mutes.
        rig_set_master_volume((int32_t)(s_pot_filtered + 0.5f), RIG_SRC_KNOB);
        rig_set_muted(false, RIG_SRC_KNOB);
    }
}
#endif

// ---------------------------------------------------------------------------------------------
// Adafruit STEMMA QT rotary encoders (seesaw firmware on an ATSAMD09). Register map from
// Adafruit's seesaw library. Written ahead of the hardware: UNTESTED until the encoders arrive.

#if CONFIG_KNOBS_ENCODERS
#define SS_STATUS_BASE      0x00
#define SS_STATUS_HW_ID     0x01
#define SS_GPIO_BASE        0x01
#define SS_GPIO_DIRCLR_BULK 0x03
#define SS_GPIO_BULK        0x04
#define SS_GPIO_BULK_SET    0x05
#define SS_GPIO_INTENSET    0x08
#define SS_GPIO_PULLENSET   0x0B
#define SS_ENCODER_BASE     0x11
#define SS_ENCODER_INTENSET 0x10
#define SS_ENCODER_DELTA    0x40
#define SS_BUTTON_PIN       24
#define SS_READ_DELAY_TICKS 2         // >= 1 ms between the register write and the read (seesaw needs it)

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
    bool present, pressed;
} encoder_t;

static encoder_t s_enc[2] = { { .addr = 0x36 }, { .addr = 0x37 } };

static esp_err_t ss_write(encoder_t *e, uint8_t base, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[8] = { base, reg };
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(e->dev, buf, 2 + len, 20);
}

static esp_err_t ss_read(encoder_t *e, uint8_t base, uint8_t reg, uint8_t *out, size_t len) {
    uint8_t cmd[2] = { base, reg };
    esp_err_t err = i2c_master_transmit(e->dev, cmd, 2, 20);
    if (err != ESP_OK) return err;
    // Release the bus while the seesaw prepares its answer, so touch reads aren't held up.
    vTaskDelay(SS_READ_DELAY_TICKS);
    return i2c_master_receive(e->dev, out, len, 20);
}

static void encoder_init(encoder_t *e, i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = e->addr, .scl_speed_hz = 400000 };
    if (i2c_master_probe(bus, e->addr, 20) != ESP_OK || i2c_master_bus_add_device(bus, &cfg, &e->dev) != ESP_OK) return;
    uint8_t id = 0;
    if (ss_read(e, SS_STATUS_BASE, SS_STATUS_HW_ID, &id, 1) != ESP_OK) return;
    // Button pin as input with pull-up, with interrupts for it and for rotation.
    // seesaw GPIO masks are 32-bit big-endian: pin 24 is bit 0 of the first byte.
    const uint8_t mask[4] = { (uint8_t)(1u << (SS_BUTTON_PIN - 24)), 0, 0, 0 };
    ss_write(e, SS_GPIO_BASE, SS_GPIO_DIRCLR_BULK, mask, 4);
    ss_write(e, SS_GPIO_BASE, SS_GPIO_PULLENSET, mask, 4);
    ss_write(e, SS_GPIO_BASE, SS_GPIO_BULK_SET, mask, 4);
    ss_write(e, SS_GPIO_BASE, SS_GPIO_INTENSET, mask, 4);
    const uint8_t on = 1;
    ss_write(e, SS_ENCODER_BASE, SS_ENCODER_INTENSET, &on, 1);
    e->present = true;
    ESP_LOGI(TAG, "encoder at 0x%02X ready (seesaw hw id 0x%02X)", e->addr, id);
}

// The knob 2 target: the parameter open in the slider, else the selected effect's first knob.
static void param_target(uint8_t *fx, uint8_t *knob) {
    if (!ui_vertical_slider_target(fx, knob)) {
        *fx = ui_main_selected_fx();
        *knob = 0;
    }
}

static void encoder_poll(encoder_t *e, int which) {
    uint8_t b[4];
    if (ss_read(e, SS_ENCODER_BASE, SS_ENCODER_DELTA, b, 4) == ESP_OK) {
        int32_t delta = (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
#if CONFIG_KNOBS_ENCODER_REVERSE
        delta = -delta;
#endif
        if (delta) {
            if (which == 0) {
                rig_set_master_volume(rig()->master_volume + delta * 2, RIG_SRC_KNOB);
                rig_set_muted(false, RIG_SRC_KNOB);
            } else {
                uint8_t fx, k;
                param_target(&fx, &k);
                rig_set_knob(fx, k, g_knob_values[fx][k] + delta, RIG_SRC_KNOB);
            }
        }
    }
    if (ss_read(e, SS_GPIO_BASE, SS_GPIO_BULK, b, 4) == ESP_OK) {
        const bool pressed = !(b[0] & (1u << (SS_BUTTON_PIN - 24))); // pulled up: pressed reads 0
        if (pressed && !e->pressed) {
            if (which == 0) {
                rig_set_muted(!rig()->muted, RIG_SRC_KNOB);
            } else {
                const uint8_t fx = ui_main_selected_fx();
                rig_set_fx_on(fx, !rig()->on[fx], RIG_SRC_KNOB);
            }
        }
        e->pressed = pressed;
    }
}
#endif

// ---------------------------------------------------------------------------------------------

static void knobs_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
#if CONFIG_KNOBS_GPIO6_POT
        pot_poll();
#endif
#if CONFIG_KNOBS_ENCODERS
        // With INT wired, the bus is only touched when an encoder has something to report.
#if CONFIG_KNOBS_GPIO6_ENCODER_INT
        if (gpio_get_level(KNOB_GPIO) != 0) continue;
#endif
        for (int i = 0; i < 2; i++) {
            if (s_enc[i].present) encoder_poll(&s_enc[i], i);
        }
#endif
    }
}

void knobs_init(void) {
    i2c_master_bus_handle_t bus = board_i2c_bus();
    scan_bus(bus);

    bool any = false;
#if CONFIG_KNOBS_GPIO6_POT
    if (pot_init()) {
        any = true;
        ESP_LOGI(TAG, "pot on GPIO%d (J8) as master volume", KNOB_GPIO);
    } else {
        ESP_LOGE(TAG, "ADC setup on GPIO%d failed", KNOB_GPIO);
    }
#endif
#if CONFIG_KNOBS_ENCODERS
#if CONFIG_KNOBS_GPIO6_ENCODER_INT
    gpio_config_t io = { .pin_bit_mask = 1ULL << KNOB_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
#endif
    for (int i = 0; i < 2; i++) {
        encoder_init(&s_enc[i], bus);
        any |= s_enc[i].present;
    }
#endif
    if (!any) return;
    // Stack in PSRAM: internal RAM is tight, and this task never writes flash.
    xTaskCreatePinnedToCoreWithCaps(knobs_task, "knobs", 4096, NULL, TASK_PRIORITY, NULL, 0, MALLOC_CAP_SPIRAM);
}
