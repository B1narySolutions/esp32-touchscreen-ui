#include "diag.h"
#include "board_init.h"
#include "seed_link.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "diag";

static temperature_sensor_handle_t s_tsens;
static uint32_t s_touches;
static int16_t s_last_x = -1, s_last_y = -1;

// Latest hardware snapshot, written once a second by the diag task, read by diag_take().
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static diag_snapshot_t s_snap;

// UI frame timing, accumulated in the LVGL task and handed to the stream task once a
// second. All values are measured with esp_timer around LVGL's own display events.
typedef struct {
    uint32_t frames;      // refreshes that actually rendered something
    uint64_t frame_us;    // REFR_START -> REFR_READY, summed over rendered frames
    uint32_t frame_max_us;
    uint64_t flush_us;    // time inside the adapter's flush_cb (in DOUBLE_DIRECT: mostly the VSYNC wait)
    uint64_t wait_us;     // time blocked waiting for the previous flush to complete
    uint64_t inv_px;      // pixels invalidated (before LVGL merges overlapping areas)
    uint32_t tap_us;      // touch press -> end of the next rendered frame, worst case
} ui_perf_t;

static portMUX_TYPE s_perf_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_perf_t s_perf_acc;
static int64_t s_t_refr, s_t_flush, s_t_wait, s_t_press;
static bool s_rendered;

static void on_display_event(lv_event_t *e) {
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s_t_refr = now;
        s_rendered = false;
        break;
    case LV_EVENT_RENDER_READY:
        s_rendered = true;
        break;
    case LV_EVENT_FLUSH_START:
        s_t_flush = now;
        break;
    case LV_EVENT_FLUSH_FINISH:
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_acc.flush_us += now - s_t_flush;
        portEXIT_CRITICAL(&s_perf_mux);
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        s_t_wait = now;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        portENTER_CRITICAL(&s_perf_mux);
        s_perf_acc.wait_us += now - s_t_wait;
        portEXIT_CRITICAL(&s_perf_mux);
        break;
    case LV_EVENT_INVALIDATE_AREA: {
        const lv_area_t *a = lv_event_get_param(e);
        if (a) {
            portENTER_CRITICAL(&s_perf_mux);
            s_perf_acc.inv_px += lv_area_get_size(a);
            portEXIT_CRITICAL(&s_perf_mux);
        }
        break;
    }
    case LV_EVENT_REFR_READY:
        if (!s_rendered) break;
        portENTER_CRITICAL(&s_perf_mux);
        uint32_t dt = (uint32_t)(now - s_t_refr);
        s_perf_acc.frames++;
        s_perf_acc.frame_us += dt;
        if (dt > s_perf_acc.frame_max_us) s_perf_acc.frame_max_us = dt;
        if (s_t_press) {
            uint32_t lat = (uint32_t)(now - s_t_press);
            if (lat > s_perf_acc.tap_us) s_perf_acc.tap_us = lat;
            s_t_press = 0;
        }
        portEXIT_CRITICAL(&s_perf_mux);
        break;
    default:
        break;
    }
}

static void on_indev_pressed(lv_event_t *e) {
    (void)e;
    s_t_press = esp_timer_get_time();
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
    lv_display_add_event_cb(disp, on_display_event, LV_EVENT_ALL, NULL);
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        lv_indev_add_event_cb(in, on_indev_pressed, LV_EVENT_PRESSED, NULL);
    }
}

void diag_take(diag_snapshot_t *out) {
    portENTER_CRITICAL(&s_snap_mux);
    *out = s_snap;
    portEXIT_CRITICAL(&s_snap_mux);
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->touches = s_touches;
    out->last_touch_x = s_last_x;
    out->last_touch_y = s_last_y;
}

// Per-core CPU load from FreeRTOS idle-task run time. Needs run-time stats clocked by
// esp_timer (1 us ticks), which sdkconfig.defaults enables; otherwise reports NAN.
static void sample_cpu_load(float out[2]) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP
    static configRUN_TIME_COUNTER_TYPE prev_idle[2];
    static int64_t prev_wall;
    int64_t wall = esp_timer_get_time();
    for (int core = 0; core < 2; core++) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
        configRUN_TIME_COUNTER_TYPE d_idle = idle - prev_idle[core]; // unsigned: survives wrap
        prev_idle[core] = idle;
        float pct = NAN;
        if (prev_wall && wall > prev_wall) {
            pct = 100.0f - 100.0f * (float)d_idle / (float)(wall - prev_wall);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        out[core] = pct;
    }
    prev_wall = wall;
#else
    out[0] = out[1] = NAN;
#endif
}

static void sample_hardware(diag_snapshot_t *d) {
    static float batt_hist[DIAG_BATT_WINDOW_S];
    static int batt_n, batt_i;

    d->reset_reason = reset_reason_str();
    d->fw_version = esp_app_get_description()->version;
    esp_app_get_elf_sha256(d->fw_elf, sizeof(d->fw_elf));
    sample_cpu_load(d->cpu_load_pct);
    d->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    d->heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    d->heap_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    d->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    d->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    d->chip_temp_c = NAN;
    if (s_tsens) {
        float c;
        if (temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) d->chip_temp_c = c;
    }

    d->battery_v = board_battery_volts();
    if (d->battery_v >= 0) {
        batt_hist[batt_i] = d->battery_v;
        batt_i = (batt_i + 1) % DIAG_BATT_WINDOW_S;
        if (batt_n < DIAG_BATT_WINDOW_S) batt_n++;
    }
    d->battery_v_min = d->battery_v_max = -1.0f;
    for (int i = 0; i < batt_n; i++) {
        if (d->battery_v_min < 0 || batt_hist[i] < d->battery_v_min) d->battery_v_min = batt_hist[i];
        if (batt_hist[i] > d->battery_v_max) d->battery_v_max = batt_hist[i];
    }

    d->backlight_pct = board_get_backlight_pct();
    d->panel_refresh_hz = board_panel_refresh_hz();

    static uint32_t exio_recoveries;
    uint8_t pins;
    bool recovered;
    d->exio_pins = board_check_expander(&pins, &recovered) == ESP_OK ? pins : -1;
    if (recovered) exio_recoveries++;
    d->exio_recoveries = exio_recoveries;
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

static void json_float(char *buf, size_t n, const char *fmt, float v) {
    if (isnan(v)) snprintf(buf, n, "null");
    else snprintf(buf, n, fmt, v);
}

// LVGL watchdog. The LVGL task holds the adapter lock for the whole of each refresh, and in
// DOUBLE_DIRECT mode it blocks inside flush until the panel's VSYNC. If the RGB scan-out ever
// stops, LVGL blocks forever: the screen goes black and touch stops being processed, while
// everything else keeps running. Detect that by probing the lock; after 2 s, dump the task
// states once and restart the RGB scan-out.
static uint32_t s_lvgl_stuck_s, s_rgb_restarts;

static void check_lvgl(void) {
    if (board_lvgl_try_lock(300)) {
        board_lvgl_unlock();
        if (s_lvgl_stuck_s) ESP_LOGW(TAG, "LVGL task responsive again after %lus", (unsigned long)s_lvgl_stuck_s);
        s_lvgl_stuck_s = 0;
        return;
    }
    s_lvgl_stuck_s++;
    ESP_LOGW(TAG, "LVGL task unresponsive for %lus", (unsigned long)s_lvgl_stuck_s);
    if (s_lvgl_stuck_s == 2) {
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
        static char list[2048];
        vTaskList(list);
        printf("TASKS (name state prio stack_free num core)\n%s", list);
#endif
    }
    if (s_lvgl_stuck_s >= 2) {
        esp_err_t err = board_restart_rgb();
        s_rgb_restarts++;
        ESP_LOGW(TAG, "restarted RGB scan-out (%s), restart #%lu", esp_err_to_name(err), (unsigned long)s_rgb_restarts);
    }
}

static void stream_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        check_lvgl();

        portENTER_CRITICAL(&s_perf_mux);
        ui_perf_t p = s_perf_acc;
        memset(&s_perf_acc, 0, sizeof(s_perf_acc));
        portEXIT_CRITICAL(&s_perf_mux);
        uint32_t nf = p.frames ? p.frames : 1;

        diag_snapshot_t d = { 0 };
        sample_hardware(&d);
        d.redraws_per_s = p.frames;
        d.frame_avg_ms = p.frame_us / 1000.0f / nf;
        d.frame_max_ms = p.frame_max_us / 1000.0f;
        d.tap_ms = p.tap_us / 1000.0f;
        d.lvgl_stuck_s = s_lvgl_stuck_s;
        d.rgb_restarts = s_rgb_restarts;
        portENTER_CRITICAL(&s_snap_mux);
        s_snap = d;
        portEXIT_CRITICAL(&s_snap_mux);
        diag_take(&d); // adds the live uptime and touch fields

        seed_link_stats_t s;
        seed_link_get_stats(&s);
        char temp[12], cpu0[12], cpu1[12];
        json_float(temp, sizeof(temp), "%.1f", d.chip_temp_c);
        json_float(cpu0, sizeof(cpu0), "%.1f", d.cpu_load_pct[0]);
        json_float(cpu1, sizeof(cpu1), "%.1f", d.cpu_load_pct[1]);

        // One line, machine-readable. The preview's Test Mode parses lines starting "DIAG ".
        printf("DIAG {\"up\":%lu,\"rst\":\"%s\",\"idf\":\"%s\",\"fw\":\"%s\",\"elf\":\"%s\","
               "\"cpu\":[%s,%s],"
               "\"heap\":%lu,\"heap_min\":%lu,\"heap_big\":%lu,\"psram\":%lu,\"psram_total\":%lu,"
               "\"temp\":%s,\"vbat\":%.2f,\"vbat_min\":%.2f,\"vbat_max\":%.2f,\"bl\":%u,"
               "\"exio\":%d,\"exio_lost\":%lu,\"lvgl_stuck\":%lu,\"rgb_restarts\":%lu,"
               "\"panel_hz\":%.1f,\"fps\":%lu,"
               "\"touches\":%lu,\"tx\":%d,\"ty\":%d,"
               "\"ui\":{\"frame_avg\":%.1f,\"frame_max\":%.1f,\"flush\":%.1f,\"wait\":%.1f,"
               "\"inv_kpx\":%lu,\"tap\":%.1f},"
               "\"seed\":{\"conn\":%s,\"tx\":%lu,\"rx\":%lu,\"err\":%lu,\"rtt\":%.2f,"
               "\"fw\":\"%s\",\"sr\":%lu,\"blk\":%u,\"cpu\":%.1f,\"nam\":\"%s\","
               "\"inpk\":%.1f,\"outpk\":%.1f,\"clips\":%lu}}\n",
               (unsigned long)d.uptime_s, d.reset_reason, esp_get_idf_version(), d.fw_version, d.fw_elf,
               cpu0, cpu1,
               (unsigned long)d.heap_free, (unsigned long)d.heap_min_free,
               (unsigned long)d.heap_largest_block, (unsigned long)d.psram_free,
               (unsigned long)d.psram_total,
               temp, d.battery_v, d.battery_v_min, d.battery_v_max, d.backlight_pct,
               d.exio_pins, (unsigned long)d.exio_recoveries,
               (unsigned long)d.lvgl_stuck_s, (unsigned long)d.rgb_restarts,
               d.panel_refresh_hz, (unsigned long)d.redraws_per_s,
               (unsigned long)d.touches, d.last_touch_x, d.last_touch_y,
               d.frame_avg_ms, d.frame_max_ms, p.flush_us / 1000.0 / nf,
               p.wait_us / 1000.0 / nf, (unsigned long)(p.inv_px / 1000), d.tap_ms,
               s.connected ? "true" : "false", (unsigned long)s.packets_tx,
               (unsigned long)s.packets_rx, (unsigned long)s.link_errors, s.rtt_ms,
               s.dsp_fw_version, (unsigned long)s.sample_rate_hz, s.block_size, s.dsp_cpu_pct,
               s.nam_model, s.input_peak_dbfs, s.output_peak_dbfs, (unsigned long)s.clip_count);
    }
}

void diag_start_serial_stream(void) {
    diag_snapshot_t d = { 0 };
    sample_hardware(&d); // so Test Mode has real values before the first tick
    portENTER_CRITICAL(&s_snap_mux);
    s_snap = d;
    portEXIT_CRITICAL(&s_snap_mux);
    xTaskCreate(stream_task, "diag_stream", 6144, NULL, 1, NULL); // printf of floats is stack-hungry
}
