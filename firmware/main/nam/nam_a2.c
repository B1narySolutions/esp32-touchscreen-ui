/*
 * nam_a2: see nam_a2.h. A port of realtime-nam-seed3/scripts/convert_a2.py; comments name the
 * Python each step mirrors.
 */
#include "nam_a2.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seed_link_proto.h"  // slp_crc32

#define MAX_DEPTH 64

// ---------------------------------------------------------------------------------------------
// In-place JSON walking. Every function takes a pointer to the first character of a value
// (whitespace already skipped) and relies on the NUL sentinel at the end of the text: no token
// may contain a NUL, so scanning always stops there.

enum { J_BAD, J_OBJ, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL };

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int kind(const char *p) {
    switch (*p) {
    case '{': return J_OBJ;
    case '[': return J_ARR;
    case '"': return J_STR;
    case 't': return J_TRUE;
    case 'f': return J_FALSE;
    case 'n': return J_NULL;
    case '-': case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': case 'N': case 'I':
        return J_NUM;
    default: return J_BAD;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Length of a valid UTF-8 sequence starting at p (strict, as Python's decoder), or 0.
static int utf8_len(const unsigned char *p) {
    if (p[0] < 0x80) return 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF) return (p[1] & 0xC0) == 0x80 ? 2 : 0;
    if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (p[0] == 0xE0 && p[1] < 0xA0) return 0;  // overlong
        if (p[0] == 0xED && p[1] >= 0xA0) return 0; // surrogate
        return 3;
    }
    if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (p[0] == 0xF0 && p[1] < 0x90) return 0;
        if (p[0] == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

// p at the opening quote. Returns the position after the closing quote, or NULL.
static const char *skip_string(const char *p) {
    p++;
    for (;;) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL; // control characters (and the NUL sentinel) are invalid
        if (c == '\\') {
            p++;
            if (*p == 'u') {
                for (int i = 1; i <= 4; i++) if (hexval(p[i]) < 0) return NULL;
                p += 5;
            } else if (*p && strchr("\"\\/bfnrt", *p)) {
                p++;
            } else {
                return NULL;
            }
        } else {
            const int n = utf8_len((const unsigned char *)p);
            if (!n) return NULL;
            p += n;
        }
    }
}

// A JSON number, or Python's extensions NaN, Infinity and -Infinity. Returns the end, or NULL.
static const char *scan_number(const char *p, bool *is_int) {
    const char *s = p;
    *is_int = false;
    if (*p == '-') p++;
    if (strncmp(p, "Infinity", 8) == 0) return p + 8;
    if (p == s && strncmp(p, "NaN", 3) == 0) return p + 3;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
    } else {
        return NULL;
    }
    *is_int = true;
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9')) return NULL;
        while (*p >= '0' && *p <= '9') p++;
        *is_int = false;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return NULL;
        while (*p >= '0' && *p <= '9') p++;
        *is_int = false;
    }
    return p;
}

static double number_value(const char *p) {
    // strtod also understands NaN and Infinity; the span was validated by scan_number, and the
    // character after it is always a JSON delimiter strtod won't consume.
    return strtod(p, NULL);
}

// Returns the position after the value starting at p, or NULL if it isn't valid JSON.
static const char *skip_value(const char *p, int depth) {
    if (depth > MAX_DEPTH) return NULL;
    bool is_int;
    switch (kind(p)) {
    case J_STR: return skip_string(p);
    case J_NUM: return scan_number(p, &is_int);
    case J_TRUE: return strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
    case J_FALSE: return strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
    case J_NULL: return strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
    case J_ARR:
        p = ws(p + 1);
        if (*p == ']') return p + 1;
        for (;;) {
            p = skip_value(p, depth + 1);
            if (!p) return NULL;
            p = ws(p);
            if (*p == ']') return p + 1;
            if (*p != ',') return NULL;
            p = ws(p + 1);
        }
    case J_OBJ:
        p = ws(p + 1);
        if (*p == '}') return p + 1;
        for (;;) {
            if (*p != '"' || !(p = skip_string(p))) return NULL;
            p = ws(p);
            if (*p != ':') return NULL;
            p = skip_value(ws(p + 1), depth + 1);
            if (!p) return NULL;
            p = ws(p);
            if (*p == '}') return p + 1;
            if (*p != ',') return NULL;
            p = ws(p + 1);
        }
    default: return NULL;
    }
}

// Decodes the next code point of a validated string (p inside the quotes). Returns -1 at the
// closing quote. Lone surrogates come through as themselves, as in Python.
static long next_cp(const char **pp) {
    const char *p = *pp;
    long cp;
    if (*p == '"') return -1;
    if (*p == '\\') {
        p++;
        switch (*p) {
        case 'b': cp = '\b'; p++; break;
        case 'f': cp = '\f'; p++; break;
        case 'n': cp = '\n'; p++; break;
        case 'r': cp = '\r'; p++; break;
        case 't': cp = '\t'; p++; break;
        case 'u':
            cp = (hexval(p[1]) << 12) | (hexval(p[2]) << 8) | (hexval(p[3]) << 4) | hexval(p[4]);
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                long lo = (hexval(p[2]) << 12) | (hexval(p[3]) << 8) | (hexval(p[4]) << 4) | hexval(p[5]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            break;
        default: cp = (unsigned char)*p; p++; break; // " \ /
        }
    } else {
        const unsigned char *u = (const unsigned char *)p;
        const int n = utf8_len(u);
        if (n == 1) cp = u[0];
        else if (n == 2) cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        else if (n == 3) cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        else cp = ((long)(u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        p += n;
    }
    *pp = p;
    return cp;
}

static bool str_eq(const char *a, const char *b) {
    a++;
    b++;
    for (;;) {
        const long x = next_cp(&a), y = next_cp(&b);
        if (x != y) return false;
        if (x < 0) return true;
    }
}

static bool str_eq_c(const char *a, const char *s) {
    a++;
    for (;;) {
        const long x = next_cp(&a);
        if (x < 0) return *s == '\0';
        if (*s == '\0' || x != (unsigned char)*s) return false;
        s++;
    }
}

// Copies a JSON string's text into out (ASCII-ish; other code points become '?').
static void str_copy(const char *a, char *out, size_t cap) {
    size_t n = 0;
    a++;
    for (long c; (c = next_cp(&a)) >= 0;) {
        if (n + 1 < cap) out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[n] = '\0';
}

// Iterates an object's members. it starts just after '{'; returns false at the end.
typedef struct { const char *p; } obj_it_t;

static void obj_begin(obj_it_t *it, const char *obj) { it->p = ws(obj + 1); }

static bool obj_next(obj_it_t *it, const char **key, const char **value) {
    if (*it->p == '}') return false;
    *key = it->p;
    const char *p = ws(skip_string(it->p));
    *value = ws(p + 1);
    p = ws(skip_value(*value, 0));
    it->p = *p == ',' ? ws(p + 1) : p;
    return true;
}

// dict[key] with Python's rule that the last duplicate wins. NULL if absent.
static const char *obj_get(const char *obj, const char *key) {
    obj_it_t it;
    const char *k, *v, *found = NULL;
    obj_begin(&it, obj);
    while (obj_next(&it, &k, &v)) if (str_eq_c(k, key)) found = v;
    return found;
}

// Is this the last occurrence of its key in the object (so it's the one Python keeps)?
static bool is_last_key(const char *obj, const char *key_at) {
    obj_it_t it;
    const char *k, *v;
    bool seen = false;
    obj_begin(&it, obj);
    while (obj_next(&it, &k, &v)) {
        if (k == key_at) seen = true;
        else if (seen && str_eq(k, key_at)) return false;
    }
    return true;
}

static int distinct_keys(const char *obj) {
    obj_it_t it;
    const char *k, *v;
    int n = 0;
    obj_begin(&it, obj);
    while (obj_next(&it, &k, &v)) if (is_last_key(obj, k)) n++;
    return n;
}

static int array_len(const char *arr) {
    const char *p = ws(arr + 1);
    int n = 0;
    if (*p == ']') return 0;
    for (;;) {
        n++;
        p = ws(skip_value(p, 0));
        if (*p == ']') return n;
        p = ws(p + 1);
    }
}

// Python truthiness of a value used as "metadata or {}".
static bool is_falsy(const char *v) {
    switch (kind(v)) {
    case J_NULL: case J_FALSE: return true;
    case J_NUM: return number_value(v) == 0.0;
    case J_STR: return v[1] == '"';
    case J_ARR: return *ws(v + 1) == ']';
    case J_OBJ: return *ws(v + 1) == '}';
    default: return false;
    }
}

// Python ==, with True == 1, False == 0, and numbers compared by value.
static bool num_like(const char *v, double *out) {
    switch (kind(v)) {
    case J_NUM: *out = number_value(v); return true;
    case J_TRUE: *out = 1.0; return true;
    case J_FALSE: *out = 0.0; return true;
    default: return false;
    }
}

static bool py_eq(const char *a, const char *b, int depth) {
    if (depth > MAX_DEPTH) return false;
    double x, y;
    if (num_like(a, &x) && num_like(b, &y)) return x == y;
    const int ka = kind(a), kb = kind(b);
    if (ka != kb) return false;
    switch (ka) {
    case J_NULL: return true;
    case J_STR: return str_eq(a, b);
    case J_ARR: {
        const char *p = ws(a + 1), *q = ws(b + 1);
        for (;;) {
            if (*p == ']' || *q == ']') return *p == ']' && *q == ']';
            if (!py_eq(p, q, depth + 1)) return false;
            p = ws(skip_value(p, 0));
            q = ws(skip_value(q, 0));
            if (*p == ',') p = ws(p + 1);
            if (*q == ',') q = ws(q + 1);
        }
    }
    case J_OBJ: {
        if (distinct_keys(a) != distinct_keys(b)) return false;
        obj_it_t it;
        const char *k, *v;
        obj_begin(&it, a);
        while (obj_next(&it, &k, &v)) {
            if (!is_last_key(a, k)) continue;
            // Find the same key in b (its last occurrence).
            obj_it_t jt;
            const char *k2, *v2, *match = NULL;
            obj_begin(&jt, b);
            while (obj_next(&jt, &k2, &v2)) if (str_eq(k, k2)) match = v2;
            if (!match || !py_eq(v, match, depth + 1)) return false;
        }
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------------------------
// convert_a2.py

static nam_err_t fail(nam_err_t e, char *reason, size_t cap, const char *fmt, ...) {
    if (reason && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(reason, cap, fmt, ap);
        va_end(ap);
    }
    return e;
}

// to_float32(): type int or float (not bool), finite, and finite after rounding to float32.
static bool to_float32(const char *v, float *out) {
    if (kind(v) != J_NUM) return false;
    const double d = number_value(v);
    if (!isfinite(d)) return false;
    const float f = (float)d;
    if (!isfinite(f)) return false;
    *out = f;
    return true;
}

// The submodel's metadata merged over the container's ({**c, **m}); only its name is kept.
static bool metadata_name(const char *meta, char *name, size_t cap) {
    if (!meta || is_falsy(meta)) return true;
    if (kind(meta) != J_OBJ) return false; // {**[1]} is a TypeError in Python
    const char *n = obj_get(meta, "name");
    if (n && kind(n) == J_STR) str_copy(n, name, cap);
    return true;
}

nam_err_t nam_a2_load(const char *json, size_t len, nam_a2_t *out, char *reason, size_t cap) {
    memset(out, 0, sizeof(*out));
    if (reason && cap) reason[0] = '\0';

    // json.loads(raw): the whole document must be valid (a UTF-8 BOM is accepted, as Python's
    // encoding detection does).
    const char *root = json;
    if (len >= 3 && (unsigned char)root[0] == 0xEF && (unsigned char)root[1] == 0xBB && (unsigned char)root[2] == 0xBF) root += 3;
    root = ws(root);
    const char *end = skip_value(root, 0);
    if (!end || ws(end) != json + len) {
        return fail(NAM_ERR_JSON, reason, cap, "not a valid .nam file (bad JSON)");
    }
    if (kind(root) != J_OBJ) return fail(NAM_ERR_NOT_MODEL, reason, cap, "not a NAM model");

    // extract(): an A2 SlimmableContainer holds the Lite model as its max_value == 0.5 submodel.
    const char *model = root, *sample_rate = obj_get(root, "sample_rate");
    const char *arch = obj_get(root, "architecture");
    const bool container = arch && kind(arch) == J_STR && str_eq_c(arch, "SlimmableContainer");
    if (!container) {
        // A bare model: Python never merges its metadata, so only read a name if there is one.
        const char *meta = obj_get(root, "metadata");
        if (meta && kind(meta) == J_OBJ) metadata_name(meta, out->name, sizeof(out->name));
    } else if (!metadata_name(obj_get(root, "metadata"), out->name, sizeof(out->name))) {
        return fail(NAM_ERR_NOT_MODEL, reason, cap, "malformed metadata");
    }
    if (container) {
        const char *config = obj_get(root, "config");
        const char *subs = config && kind(config) == J_OBJ ? obj_get(config, "submodels") : NULL;
        if (!subs || kind(subs) != J_ARR) return fail(NAM_ERR_CONTAINER, reason, cap, "A2 container without submodels");
        const char *lite = NULL;
        int lites = 0;
        const char *p = ws(subs + 1);
        static const char half[] = "0.5";
        while (*p != ']') {
            if (kind(p) != J_OBJ) return fail(NAM_ERR_CONTAINER, reason, cap, "malformed A2 container");
            const char *mv = obj_get(p, "max_value");
            if (mv && py_eq(mv, half, 0)) {
                lite = obj_get(p, "model");
                if (!lite) return fail(NAM_ERR_CONTAINER, reason, cap, "A2 container submodel without a model");
                lites++;
            }
            p = ws(skip_value(p, 0));
            if (*p == ',') p = ws(p + 1);
        }
        if (lites != 1) {
            return fail(NAM_ERR_CONTAINER, reason, cap, lites ? "A2 container with several Lite submodels"
                                                              : "A2 container without a Lite (0.5) submodel");
        }
        if (kind(lite) != J_OBJ) return fail(NAM_ERR_CONTAINER, reason, cap, "malformed A2 container");
        model = lite;
        // The Lite model inherits the container's sample rate when it lacks its own (or is null).
        const char *own = obj_get(model, "sample_rate");
        if (own && kind(own) != J_NULL) sample_rate = own;
        if (!metadata_name(obj_get(model, "metadata"), out->name, sizeof(out->name))) {
            return fail(NAM_ERR_NOT_MODEL, reason, cap, "malformed metadata");
        }
        arch = obj_get(model, "architecture");
    }

    // validate(): NAM 0.7.0, 48 kHz WaveNet. Python raises one error for all three; the reason
    // here says which, since that's what a user needs to know.
    static const char v070[] = "\"0.7.0\"", sr48k[] = "48000";
    const char *version = obj_get(model, "version");
    if (!arch || kind(arch) != J_STR || !str_eq_c(arch, "WaveNet")) {
        char a[32] = "unknown";
        if (arch && kind(arch) == J_STR) str_copy(arch, a, sizeof(a));
        return fail(NAM_ERR_ARCH, reason, cap, "%s model, not an A2 WaveNet", a);
    }
    if (!version || !py_eq(version, v070, 0)) {
        char v[24] = "?";
        if (version && kind(version) == J_STR) str_copy(version, v, sizeof(v));
        return fail(NAM_ERR_VERSION, reason, cap, "NAM %s model (A1 / standard), not A2", v);
    }
    if (!sample_rate || !py_eq(sample_rate, sr48k, 0)) {
        double sr;
        if (sample_rate && num_like(sample_rate, &sr)) return fail(NAM_ERR_SAMPLE_RATE, reason, cap, "%.0f Hz model, needs 48 kHz", sr);
        return fail(NAM_ERR_SAMPLE_RATE, reason, cap, "no sample rate, needs 48 kHz");
    }
    const char *config = obj_get(model, "config");
    if (!config || kind(config) != J_OBJ || distinct_keys(config) != 3) {
        return fail(NAM_ERR_CONFIG, reason, cap, "not the A2-Lite layout");
    }
    const char *layers = obj_get(config, "layers"), *head = obj_get(config, "head");
    const char *head_scale = obj_get(config, "head_scale");
    if (!layers || !head || !head_scale || kind(layers) != J_ARR || kind(head) != J_NULL) {
        return fail(NAM_ERR_CONFIG, reason, cap, "not the A2-Lite layout");
    }
    // config["layers"] == [expected_layer()]
    if (array_len(layers) != 1 || !py_eq(ws(layers + 1), nam_a2_expected_layer, 0)) {
        return fail(NAM_ERR_CONFIG, reason, cap, "not the A2-Lite layer layout");
    }
    const char *weights = obj_get(model, "weights");
    if (!weights || (kind(weights) != J_ARR && kind(weights) != J_STR && kind(weights) != J_OBJ)) {
        return fail(NAM_ERR_WEIGHT_COUNT, reason, cap, "no weights");
    }
    if (kind(weights) != J_ARR) return fail(NAM_ERR_WEIGHT_VALUE, reason, cap, "invalid weights");
    const int count = array_len(weights);
    if (count != NAM_A2_WEIGHT_COUNT) {
        return fail(NAM_ERR_WEIGHT_COUNT, reason, cap, "%d weights, A2-Lite has %d", count, NAM_A2_WEIGHT_COUNT);
    }

    // to_float32() on every weight, in file order, written straight to its pack() position:
    // pack() keeps every block in place and only reorders within the conv-tap and head-tap
    // blocks ([out][in][tap] -> [tap][in][out], [in][tap] -> [tap][in]), so no scratch copy.
    const int C = NAM_A2_CHANNELS, tail = 3 * C + C * C;
    const char *p = ws(weights + 1);
    int i = 0, base = 0;
#define NEXT_WEIGHT(dst)                                                                            do {                                                                                                if (!to_float32(p, &(dst))) {                                                                       return fail(NAM_ERR_WEIGHT_VALUE, reason, cap, "weight %d is not a finite float32", i);         }                                                                                               p = ws(skip_value(p, 0));                                                                       if (*p == ',') p = ws(p + 1);                                                                   i++;                                                                                        } while (0)
    for (int k = 0; k < C; k++) NEXT_WEIGHT(out->weights[base + k]);          // input projection
    base += C;
    for (int l = 0; l < NAM_A2_LAYERS; l++) {
        const int k = nam_a2_kernels[l];
        for (int ch = 0; ch < C; ch++)
            for (int in = 0; in < C; in++)
                for (int tap = 0; tap < k; tap++) NEXT_WEIGHT(out->weights[base + (tap * C + in) * C + ch]);
        base += C * C * k;
        for (int t = 0; t < tail; t++) NEXT_WEIGHT(out->weights[base + t]);   // bias, conditioning, residual
        base += tail;
    }
    for (int in = 0; in < C; in++)
        for (int tap = 0; tap < NAM_A2_HEAD_TAPS; tap++) NEXT_WEIGHT(out->weights[base + tap * C + in]);
    base += NAM_A2_HEAD_TAPS * C;
    while (base < NAM_A2_WEIGHT_COUNT) NEXT_WEIGHT(out->weights[base++]);    // head bias and scale
#undef NEXT_WEIGHT

    // check_head_scale(): the scale is stored twice; the engine uses the weight-stream copy
    // (the last weight, which pack() leaves last).
    float scale;
    if (!to_float32(head_scale, &scale)) return fail(NAM_ERR_WEIGHT_VALUE, reason, cap, "invalid head_scale");
    if (scale != out->weights[NAM_A2_WEIGHT_COUNT - 1]) {
        return fail(NAM_ERR_HEAD_SCALE, reason, cap, "head_scale does not match the weight stream");
    }

    // Little-endian float32 bytes: both the ESP32 and the Seed are little-endian.
    out->crc32 = slp_crc32(0, (const uint8_t *)out->weights, sizeof(out->weights));
    return NAM_OK;
}
