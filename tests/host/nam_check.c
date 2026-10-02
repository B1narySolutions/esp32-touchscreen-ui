// nam_check <file.nam> [--dump out.a2l]: runs the firmware's .nam loader on one file and prints
// "OK <crc32> <name>" or "ERR <code> <reason>". Used by tools/nam_diff_test.py to compare every
// accept/reject decision with convert_a2.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nam_a2.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nam_check file.nam [--dump out.a2l]\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

    static nam_a2_t model;
    char reason[96];
    nam_err_t err = nam_a2_load(buf, got, &model, reason, sizeof(reason));
    if (err == NAM_OK) {
        printf("OK %08x %s\n", (unsigned)model.crc32, model.name);
        if (argc >= 4 && strcmp(argv[2], "--dump") == 0) {
            FILE *o = fopen(argv[3], "wb");
            fwrite(model.weights, 1, sizeof(model.weights), o);
            fclose(o);
        }
    } else {
        printf("ERR %d %s\n", (int)err, reason);
    }
    free(buf);
    return 0;
}
