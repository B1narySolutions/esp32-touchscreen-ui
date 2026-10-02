#include "nam_store.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "amp_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nam_a2.h"
#include "sd_storage.h"
#include "seed_link_proto.h"

static const char *TAG = "nam_store";

#define NAM_DIR        SD_MOUNT_POINT "/nam"
#define CACHE_DIR      NAM_DIR "/.cache"
#define MAX_FILE_BYTES (2 * 1024 * 1024)   // TONE3000 A2 files are ~300 KB
#define CACHE_MAGIC    0x314C3241u          // "A2L1"

// Cache file: this header, then the packed weights (little-endian float32).
typedef struct {
    uint32_t magic;
    uint32_t crc32;
    char name[64];
    char source[64];
} cache_header_t;

typedef struct {
    uint32_t hash;
    char name[64];
    float weights[NAM_A2_WEIGHT_COUNT];
} profile_t;

// All in PSRAM: up to AMP_MODELS_MAX_SD profiles of ~7.6 KB each.
static profile_t *s_profiles;
static int s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static nam_store_status_t s_status = { .scanning = true, .status = "reading the SD card..." };

static void set_status(bool scanning, bool card_ok, int ok, int bad, const char *fmt, ...) {
    nam_store_status_t st = { .scanning = scanning, .card_ok = card_ok, .accepted = ok, .rejected = bad };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.status, sizeof(st.status), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_mux);
    s_status = st;
    portEXIT_CRITICAL(&s_mux);
}

void nam_store_status(nam_store_status_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

const float *nam_store_weights(uint32_t hash, char *name, size_t name_len) {
    for (int i = 0; i < s_count; i++) {
        if (s_profiles[i].hash == hash) {
            if (name) strlcpy(name, s_profiles[i].name, name_len);
            return s_profiles[i].weights;
        }
    }
    return NULL;
}

// File name without directory and ".nam", for profiles whose file has no metadata name.
static void display_name(const char *file, const char *meta, char *out, size_t cap) {
    if (meta && meta[0]) {
        strlcpy(out, meta, cap);
        return;
    }
    strlcpy(out, file, cap);
    char *dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".nam") == 0) *dot = '\0';
}

static bool read_cache(const char *path, profile_t *p, uint32_t *crc_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    cache_header_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CACHE_MAGIC &&
              fread(p->weights, sizeof(p->weights), 1, f) == 1;
    fclose(f);
    // A torn or tampered cache file is just ignored and rebuilt from the .nam.
    if (!ok || slp_crc32(0, (const uint8_t *)p->weights, sizeof(p->weights)) != h.crc32) return false;
    h.name[sizeof(h.name) - 1] = '\0';
    strlcpy(p->name, h.name, sizeof(p->name));
    *crc_out = h.crc32;
    return true;
}

static void write_cache(const char *path, const profile_t *p, const char *source) {
    cache_header_t h = { .magic = CACHE_MAGIC, .crc32 = p->hash };
    strlcpy(h.name, p->name, sizeof(h.name));
    strlcpy(h.source, source, sizeof(h.source));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "can't write %s (card read-only?)", path);
        return;
    }
    const bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(p->weights, sizeof(p->weights), 1, f) == 1;
    fclose(f);
    if (!ok) remove(path);
}

// SHA-256 of the whole file (the cache key) and its contents in a PSRAM buffer, NUL-terminated.
static char *read_file(const char *path, size_t *len, uint8_t sha[32], const char **why) {
    struct stat st;
    if (stat(path, &st) != 0) { *why = "can't read the file"; return NULL; }
    if (st.st_size > MAX_FILE_BYTES) { *why = "file too large for a .nam (over 2 MB)"; return NULL; }
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { *why = "out of memory"; return NULL; }
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    if (f) fclose(f);
    if (n != (size_t)st.st_size) {
        heap_caps_free(buf);
        *why = "read error";
        return NULL;
    }
    buf[n] = '\0';
    mbedtls_sha256((const unsigned char *)buf, n, sha, 0);
    *len = n;
    return buf;
}

static void scan(void) {
    const int64_t t0 = esp_timer_get_time();
    set_status(true, false, 0, 0, "reading the SD card...");
    if (!sd_mount()) {
        set_status(false, false, 0, 0, "SD card: %s", sd_status());
        return;
    }
    DIR *dir = opendir(NAM_DIR);
    if (!dir) {
        set_status(false, true, 0, 0, "no /nam folder on the SD card");
        ESP_LOGI(TAG, "no %s folder", NAM_DIR);
        return;
    }
    mkdir(CACHE_DIR, 0775);

    s_count = 0; // a rescan rewrites the profiles; nothing is served until each is ready again
    int n_list = 0, accepted = 0, rejected = 0, from_cache = 0;
    amp_model_t *list = heap_caps_calloc(AMP_MODELS_MAX_SD, sizeof(amp_model_t), MALLOC_CAP_SPIRAM);
    nam_a2_t *conv = heap_caps_malloc(sizeof(*conv), MALLOC_CAP_SPIRAM);
    struct dirent *e;
    while (list && conv && (e = readdir(dir)) != NULL && n_list < AMP_MODELS_MAX_SD) {
        const char *dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".nam") != 0) continue;
        char path[300], cache[300];
        snprintf(path, sizeof(path), "%s/%s", NAM_DIR, e->d_name);

        amp_model_t *m = &list[n_list];
        memset(m, 0, sizeof(*m));
        profile_t *p = &s_profiles[accepted];
        const char *why = NULL;
        char reason[96];
        size_t len = 0;
        uint8_t sha[32];
        char *json = read_file(path, &len, sha, &why);
        if (json) {
            char hex[65];
            for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", sha[i]);
            snprintf(cache, sizeof(cache), "%s/%s.a2l", CACHE_DIR, hex);
            uint32_t crc;
            if (read_cache(cache, p, &crc)) {
                p->hash = crc;
                from_cache++;
            } else {
                if (nam_a2_load(json, len, conv, reason, sizeof(reason)) == NAM_OK) {
                    p->hash = conv->crc32;
                    memcpy(p->weights, conv->weights, sizeof(p->weights));
                    display_name(e->d_name, conv->name, p->name, sizeof(p->name));
                    write_cache(cache, p, e->d_name);
                } else {
                    why = reason;
                }
            }
            heap_caps_free(json);
        }

        if (why) {
            display_name(e->d_name, NULL, m->name, sizeof(m->name));
            snprintf(m->desc, sizeof(m->desc), "Can't use: %.58s.", why);
            m->rejected = true;
            rejected++;
            ESP_LOGW(TAG, "%s rejected: %s", e->d_name, why);
        } else if (nam_store_weights(p->hash, NULL, 0)) {
            // Same weights as a profile already listed (only [0, accepted) is published).
            ESP_LOGI(TAG, "%s duplicates an earlier profile; skipped", e->d_name);
            continue;
        } else {
            strlcpy(m->name, p->name, sizeof(m->name));
            snprintf(m->desc, sizeof(m->desc), "From the SD card (A2-Lite, CRC %08lx)", (unsigned long)p->hash);
            m->sd_hash = p->hash;
            accepted++;
            s_count = accepted; // publish as we go: nam_store_weights() only reads [0, s_count)
            ESP_LOGI(TAG, "%s -> \"%s\", CRC32 %08lx", e->d_name, p->name, (unsigned long)p->hash);
        }
        n_list++;
    }
    closedir(dir);
    heap_caps_free(conv);

    if (list) amp_models_set_sd(list, n_list);
    heap_caps_free(list);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%d profile(s) ready (%d from cache), %d rejected, in %d ms", accepted, from_cache, rejected, ms);
    if (accepted || rejected) {
        set_status(false, true, accepted, rejected, "%d amp profile%s from the SD card%s", accepted,
                   accepted == 1 ? "" : "s", rejected ? ", some files rejected" : "");
    } else {
        set_status(false, true, 0, 0, "no .nam files in /nam on the SD card");
    }
}

static TaskHandle_t s_task;

static void store_task(void *arg) {
    (void)arg;
    for (;;) {
        scan();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // nam_store_rescan()
    }
}

void nam_store_rescan(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

void nam_store_start(void) {
    s_profiles = heap_caps_calloc(AMP_MODELS_MAX_SD, sizeof(profile_t), MALLOC_CAP_SPIRAM);
    if (!s_profiles) {
        set_status(false, false, 0, 0, "out of memory");
        return;
    }
    // Low priority, PSRAM stack: the card and the JSON never hold up the UI or the link.
    xTaskCreatePinnedToCoreWithCaps(store_task, "nam_store", 8192, NULL, 2, &s_task, 0, MALLOC_CAP_SPIRAM);
}
