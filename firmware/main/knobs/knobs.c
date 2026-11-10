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
#include "amp_encoder.h"
#include "esp_timer.h"
#include "qt_encoder.h"

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
// Adafruit 5880/4991 seesaw encoders, polled over the four-wire STEMMA QT bus.
// Register definitions follow Adafruit's seesaw driver. The raw direction of
// product 5880 is inverted so clockwise increases the control.
#if CONFIG_KNOBS_ENCODERS
#define SS_STATUS_BASE 0x00
#define SS_STATUS_HW_ID 0x01
#define SS_STATUS_VERSION 0x02
#define SS_STATUS_OPTIONS 0x03
#define SS_GPIO_BASE 0x01
#define SS_GPIO_DIRCLR_BULK 0x03
#define SS_GPIO_BULK 0x04
#define SS_GPIO_BULK_SET 0x05
#define SS_GPIO_INTENSET 0x08
#define SS_GPIO_PULLENSET 0x0B
#define SS_ENCODER_BASE 0x11
#define SS_ENCODER_INTENSET 0x10
#define SS_ENCODER_DELTA 0x40

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
    bool present, attempted;
    unsigned errors;
    uint32_t last_probe_ms;
    qt_encoder_button_t button;
} encoder_t;
static encoder_t s_enc[2] = {{.addr = CONFIG_KNOBS_MASTER_ADDRESS}, {.addr = CONFIG_KNOBS_PARAMETER_ADDRESS}};
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static esp_err_t ss_write(encoder_t *e, uint8_t base, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[8] = {base, reg};
    if (len > sizeof(buf) - 2)
        return ESP_ERR_INVALID_ARG;
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(e->dev, buf, 2 + len, 10);
}
static esp_err_t ss_read(encoder_t *e, uint8_t base, uint8_t reg, uint8_t *out, size_t len) {
    uint8_t cmd[2] = {base, reg};
    esp_err_t err = i2c_master_transmit(e->dev, cmd, 2, 10);
    if (err != ESP_OK)
        return err;
    TickType_t wait = pdMS_TO_TICKS(2);
    vTaskDelay(wait ? wait : 1); /* Release the bus while seesaw prepares its answer. */
    return i2c_master_receive(e->dev, out, len, 10);
}
static bool encoder_connect(encoder_t *e, i2c_master_bus_handle_t bus) {
    if (i2c_master_probe(bus, e->addr, 10) != ESP_OK)
        return false;
    if (!e->dev) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = e->addr, .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(bus, &cfg, &e->dev) != ESP_OK)
            return false;
    }
    uint8_t id = 0, options[4], version[4], sample[4];
    if (ss_read(e, SS_STATUS_BASE, SS_STATUS_HW_ID, &id, 1) != ESP_OK ||
        ss_read(e, SS_STATUS_BASE, SS_STATUS_OPTIONS, options, 4) != ESP_OK ||
        ss_read(e, SS_STATUS_BASE, SS_STATUS_VERSION, version, 4) != ESP_OK)
        return false;
    bool known_chip = id == 0x55 || (id >= 0x84 && id <= 0x89);
    uint32_t product = qt_encoder_u32(version) >> 16;
    if ((!known_chip && product != 4991 && product != 5880) ||
        !(qt_encoder_u32(options) & (1u << SS_ENCODER_BASE)))
        return false;
    const uint8_t mask[4] = {1, 0, 0, 0}; /* GPIO24, big-endian, active-low button. */
    if (ss_write(e, SS_GPIO_BASE, SS_GPIO_DIRCLR_BULK, mask, 4) != ESP_OK ||
        ss_write(e, SS_GPIO_BASE, SS_GPIO_PULLENSET, mask, 4) != ESP_OK ||
        ss_write(e, SS_GPIO_BASE, SS_GPIO_BULK_SET, mask, 4) != ESP_OK)
        return false;
#if CONFIG_KNOBS_GPIO6_ENCODER_INT
    const uint8_t enable = 1;
    if (ss_write(e, SS_GPIO_BASE, SS_GPIO_INTENSET, mask, 4) != ESP_OK ||
        ss_write(e, SS_ENCODER_BASE, SS_ENCODER_INTENSET, &enable, 1) != ESP_OK)
        return false;
#endif
    /* Discard old movement after boot/reconnect, rather than jumping the volume. */
    if (ss_read(e, SS_ENCODER_BASE, SS_ENCODER_DELTA, sample, 4) != ESP_OK ||
        ss_read(e, SS_GPIO_BASE, SS_GPIO_BULK, sample, 4) != ESP_OK)
        return false;
    memset(&e->button, 0, sizeof(e->button));
    qt_encoder_button_update(&e->button, !(sample[0] & 1u), now_ms());
    e->present = true;
    e->errors = 0;
    ESP_LOGI(TAG, "%s encoder ready at 0x%02X (seesaw 0x%02X, product %lu)",
             e == &s_enc[0] ? "master/mute" : "popup", e->addr, id, (unsigned long)product);
    return true;
}
static void encoder_poll(encoder_t *e, unsigned which) {
    uint8_t raw[4];
    bool ok = true;
    if (ss_read(e, SS_ENCODER_BASE, SS_ENCODER_DELTA, raw, 4) == ESP_OK) {
        bool reverse = false;
#if CONFIG_KNOBS_ENCODER_REVERSE
        reverse = !reverse;
#endif
        if (which == 0) {
#if CONFIG_KNOBS_MASTER_REVERSE
            reverse = !reverse;
#endif
        } else {
#if CONFIG_KNOBS_PARAMETER_REVERSE
            reverse = !reverse;
#endif
        }
        int32_t delta = qt_encoder_detents(raw, reverse);
        if (delta && !amp_encoder_turn(which, delta))
            ESP_LOGW(TAG, "encoder input queue full");
    } else
        ok = false;
    if (ss_read(e, SS_GPIO_BASE, SS_GPIO_BULK, raw, 4) == ESP_OK) {
        if (qt_encoder_button_update(&e->button, !(raw[0] & 1u), now_ms()) && !amp_encoder_press(which))
            ESP_LOGW(TAG, "encoder button queue full");
    } else
        ok = false;
    if (ok)
        e->errors = 0;
    else if (++e->errors >= 3) {
        e->present = false;
        e->last_probe_ms = now_ms();
        ESP_LOGW(TAG, "encoder at 0x%02X disconnected; retrying", e->addr);
    }
}
#endif

// ---------------------------------------------------------------------------------------------

static void knobs_task(void *arg) {
    (void)arg;
#if CONFIG_KNOBS_ENCODERS
    i2c_master_bus_handle_t bus = board_i2c_bus();
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
#if CONFIG_KNOBS_GPIO6_POT
        pot_poll();
#endif
#if CONFIG_KNOBS_ENCODERS
        for (unsigned i = 0; i < 2; i++) {
            encoder_t *e = &s_enc[i];
            if (i == 1 && e->addr == s_enc[0].addr)
                continue;
            uint32_t now = now_ms();
            if (!e->present && (!e->attempted || (uint32_t)(now - e->last_probe_ms) >= 1000)) {
                bool first = !e->attempted;
                e->attempted = true;
                e->last_probe_ms = now;
                if (!encoder_connect(e, bus) && first)
                    ESP_LOGW(TAG, "encoder missing at 0x%02X; parameter board needs A0 closed for 0x37",
                             e->addr);
            }
            if (e->present) {
#if CONFIG_KNOBS_GPIO6_ENCODER_INT
                if (gpio_get_level(KNOB_GPIO) != 0 && e->button.raw_pressed == e->button.stable_pressed)
                    continue;
#endif
                encoder_poll(e, i);
            }
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
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << KNOB_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
#endif
    if (s_enc[0].addr == s_enc[1].addr)
        ESP_LOGE(TAG, "encoders need distinct I2C addresses; parameter encoder disabled");
    any = true; /* Keep probing when the encoders are plugged in after boot. */
#endif
    if (!any)
        return;
    // Stack in PSRAM: internal RAM is tight, and this task never writes flash.
    if (xTaskCreatePinnedToCoreWithCaps(knobs_task, "knobs", 4096, NULL, TASK_PRIORITY, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "could not start physical controls task");
}
