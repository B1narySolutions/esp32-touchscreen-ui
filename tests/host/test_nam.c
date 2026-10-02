// Golden test: the firmware's .nam loader must produce exactly the bytes convert_a2.py produces
// (models/golden/*.a2l, written by tools/golden_pack.py from the amps.json downloads). Skipped
// with a note when the git-ignored models haven't been downloaded. Also: hand-made rejects.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nam_a2.h"
#include "test.h"

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static nam_a2_t s_model; // 7.5 KB: not on the stack

static nam_err_t load_text(const char *text, char *reason) {
    size_t n = strlen(text);
    char *copy = malloc(n + 1); // exact size, so ASan catches any read past the sentinel
    memcpy(copy, text, n + 1);
    nam_err_t e = nam_a2_load(copy, n, &s_model, reason, 96);
    free(copy);
    return e;
}

// Replaces the first occurrence of from with to in a heap copy of text.
static char *replace(const char *text, const char *from, const char *to) {
    const char *at = strstr(text, from);
    if (!at) return NULL;
    size_t pre = (size_t)(at - text), fl = strlen(from), tl = strlen(to), rest = strlen(at + fl);
    char *out = malloc(pre + tl + rest + 1);
    memcpy(out, text, pre);
    memcpy(out + pre, to, tl);
    memcpy(out + pre + tl, at + fl, rest + 1);
    return out;
}

static void golden(const char *amp, int *ran) {
    char path[512], gpath[512];
    snprintf(path, sizeof(path), "%s/models/download/%s-a2-container.nam", REPO_ROOT, amp);
    snprintf(gpath, sizeof(gpath), "%s/models/golden/%s.a2l", REPO_ROOT, amp);
    size_t n, gn;
    char *json = read_file(path, &n);
    char *gold = read_file(gpath, &gn);
    if (!json || !gold) {
        free(json);
        free(gold);
        return;
    }
    (*ran)++;
    char reason[96];
    nam_err_t e = nam_a2_load(json, n, &s_model, reason, sizeof(reason));
    CHECK_EQ(e, NAM_OK);
    if (e != NAM_OK) fprintf(stderr, "  %s: %s\n", amp, reason);
    CHECK_EQ(gn, sizeof(s_model.weights));
    CHECK(gn == sizeof(s_model.weights) && memcmp(gold, s_model.weights, gn) == 0);
    // Only the Fender file carries metadata.name (in the container); the others have none, and
    // the device falls back to the file name.
    if (strcmp(amp, "Fender") == 0) CHECK(strcmp(s_model.name, "SLAMMIN_FNDR_DUOVERB_V_V5_CRANKED") == 0);
    else CHECK(s_model.name[0] == '\0');

    // Rejects built from the real file, one change each. The files are compact JSON, and the
    // first match of each pattern is inside the Lite submodel (it comes first). The container's
    // own "version" is never checked by convert_a2.py, hence the longer version pattern.
    struct { const char *from, *to; nam_err_t want; } cases[] = {
        { "\"model\":{\"version\":\"0.7.0\"", "\"model\":{\"version\":\"0.5.4\"", NAM_ERR_VERSION },
        { "\"max_value\":0.5", "\"max_value\":0.25", NAM_ERR_CONTAINER },
        { "\"architecture\":\"SlimmableContainer\"", "\"architecture\":\"LSTM\"", NAM_ERR_ARCH },
        { "\"negative_slope\":0.01", "\"negative_slope\":0.02", NAM_ERR_CONFIG },
        { "\"head\":null", "\"head\":{}", NAM_ERR_CONFIG },
        { "\"weights\":[", "\"weights\":[1.0,", NAM_ERR_WEIGHT_COUNT },
        { "\"weights\":[", "\"weights\":[true,", NAM_ERR_WEIGHT_COUNT },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *m = replace(json, cases[i].from, cases[i].to);
        CHECK(m != NULL);
        if (!m) continue;
        nam_err_t got = load_text(m, reason);
        CHECK_EQ(got, cases[i].want);
        free(m);
    }
    free(json);
    free(gold);
}

void test_nam(void) {
    int ran = 0;
    golden("Fender", &ran);
    golden("Vox", &ran);
    golden("Marshall", &ran);
    if (!ran) printf("(golden models not downloaded: run tools/golden_pack.py) ");

    char reason[96];
    CHECK_EQ(load_text("", reason), NAM_ERR_JSON);
    CHECK_EQ(load_text("{", reason), NAM_ERR_JSON);
    CHECK_EQ(load_text("[1,2]", reason), NAM_ERR_NOT_MODEL);
    CHECK_EQ(load_text("{\"a\":1} x", reason), NAM_ERR_JSON);
    CHECK_EQ(load_text("{\"architecture\":\"LSTM\"}", reason), NAM_ERR_ARCH);
    CHECK_EQ(load_text("{\"architecture\":\"WaveNet\",\"version\":\"0.5.2\",\"sample_rate\":48000}", reason), NAM_ERR_VERSION);
    CHECK_EQ(load_text("{\"architecture\":\"WaveNet\",\"version\":\"0.7.0\",\"sample_rate\":44100}", reason), NAM_ERR_SAMPLE_RATE);
    CHECK(strstr(reason, "44100") != NULL);
    CHECK_EQ(load_text("{\"architecture\":\"WaveNet\",\"version\":\"0.7.0\",\"sample_rate\":48000.0,\"config\":{}}", reason), NAM_ERR_CONFIG);
    CHECK_EQ(load_text("{\"architecture\":\"SlimmableContainer\",\"config\":{\"submodels\":[]}}", reason), NAM_ERR_CONTAINER);
    // Deep nesting is refused, not a stack overflow.
    char deep[400];
    memset(deep, '[', 300);
    deep[300] = '\0';
    CHECK_EQ(load_text(deep, reason), NAM_ERR_JSON);
    // Invalid UTF-8 and control characters in strings.
    CHECK_EQ(load_text("{\"a\":\"\xC3\x28\"}", reason), NAM_ERR_JSON);
    CHECK_EQ(load_text("{\"a\":\"x\ty\"}", reason), NAM_ERR_JSON);
}
