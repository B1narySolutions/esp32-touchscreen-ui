#include "test.h"

int g_checks, g_failures;

typedef struct {
    const char *name;
    void (*fn)(void);
} test_t;

int main(void) {
    const test_t tests[] = {
        { "proto (COBS, CRC, frames, messages)", test_proto },
        { "fuzz (decoder + unpackers)", test_fuzz },
        { "coalesce", test_coalesce },
        { "nam (A2 loader, golden)", test_nam },
        { "receiver (Seed side)", test_receiver },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        const int before = g_failures;
        printf("%-40s", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        printf("%s\n", g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
