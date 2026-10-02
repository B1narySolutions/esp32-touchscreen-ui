// Minimal test harness: CHECK records a failure and keeps going; each test file registers its
// tests in main.c.
#pragma once
#include <stdio.h>

extern int g_checks, g_failures;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        g_checks++;                                                                         \
        if (!(cond)) {                                                                      \
            g_failures++;                                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                      \
    do {                                                                                    \
        long long va_ = (long long)(a), vb_ = (long long)(b);                               \
        g_checks++;                                                                         \
        if (va_ != vb_) {                                                                   \
            g_failures++;                                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__,  \
                    #a, #b, va_, vb_);                                                      \
        }                                                                                   \
    } while (0)

// Deterministic PRNG (xorshift32) so failures reproduce.
static inline unsigned test_rand(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

void test_proto(void);
void test_fuzz(void);
void test_coalesce(void);
void test_nam(void);
void test_receiver(void);
