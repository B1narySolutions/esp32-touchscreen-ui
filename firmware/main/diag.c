#include "diag.h"
#include "board_init.h"
#include "seed_link.h"

#include <math.h>
#include <stdio.h>
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "diag";

static temperature_sensor_handle_t s_tsens;
static volatile uint32_t s_redraw_count;
static uint32_t s_redraws_per_s;
static uint32_t s_touches;
static int16_t s_last_x = -1, s_last_y = -1;

static void on_refr_ready(lv_event_t *e) {
    (void)e;
    s_redraw_count++;
}

static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_USB:       return "usb";
        case ESP_RST_EXT:       return "external pin";
        default:                return "unknown";
    }
}

void diag_init(lv_display_t *disp) {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
        ESP_LOGW(TAG, "chip temperature sensor unavailable");
    }
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);
}

void diag_take(diag_snapshot_t *out) {
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->reset_reason = reset_reason_str();
    out->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->chip_temp_c = NAN;
    if (s_tsens) {
        float c;
        if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) out->chip_temp_c = c;
    }
    out->battery_v = board_battery_volts();
    out->backlight_pct = board_get_backlight_pct();
    out->redraws_per_s = s_redraws_per_s;
    out->touches = s_touches;
    out->last_touch_x = s_last_x;
    out->last_touch_y = s_last_y;
}

void diag_note_touch(int16_t x, int16_t y) {
    s_touches++;
    s_last_x = x;
    s_last_y = y;
}

void diag_reset_touches(void) {
    s_touches = 0;
    s_last_x = s_last_y = -1;
}

static void stream_task(void *arg) {
    (void)arg;
    uint32_t prev = s_redraw_count;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = s_redraw_count;
        s_redraws_per_s = now - prev;
        prev = now;

        diag_snapshot_t d;
        diag_take(&d);
        seed_link_stats_t s;
        seed_link_get_stats(&s);
        char temp[12];
        if (isnan(d.chip_temp_c)) snprintf(temp, sizeof(temp), "null");
        else snprintf(temp, sizeof(temp), "%.1f", d.chip_temp_c);

        // One line, machine-readable. The preview's Test Mode parses lines starting "DIAG ".
        printf("DIAG {\"up\":%lu,\"rst\":\"%s\",\"idf\":\"%s\","
               "\"heap\":%lu,\"heap_min\":%lu,\"heap_big\":%lu,\"psram\":%lu,\"psram_total\":%lu,"
               "\"temp\":%s,\"vbat\":%.2f,\"bl\":%u,\"fps\":%lu,"
               "\"touches\":%lu,\"tx\":%d,\"ty\":%d,"
               "\"seed\":{\"conn\":%s,\"tx\":%lu,\"rx\":%lu,\"err\":%lu,\"rtt\":%.2f,"
               "\"fw\":\"%s\",\"sr\":%lu,\"blk\":%u,\"cpu\":%.1f,\"nam\":\"%s\","
               "\"inpk\":%.1f,\"outpk\":%.1f,\"clips\":%lu}}\n",
               (unsigned long)d.uptime_s, d.reset_reason, esp_get_idf_version(),
               (unsigned long)d.heap_free, (unsigned long)d.heap_min_free,
               (unsigned long)d.heap_largest_block, (unsigned long)d.psram_free,
               (unsigned long)d.psram_total,
               temp, d.battery_v, d.backlight_pct, (unsigned long)d.redraws_per_s,
               (unsigned long)d.touches, d.last_touch_x, d.last_touch_y,
               s.connected ? "true" : "false", (unsigned long)s.packets_tx,
               (unsigned long)s.packets_rx, (unsigned long)s.link_errors, s.rtt_ms,
               s.dsp_fw_version, (unsigned long)s.sample_rate_hz, s.block_size, s.dsp_cpu_pct,
               s.nam_model, s.input_peak_dbfs, s.output_peak_dbfs, (unsigned long)s.clip_count);
    }
}

void diag_start_serial_stream(void) {
    xTaskCreate(stream_task, "diag_stream", 4096, NULL, 1, NULL);
}
