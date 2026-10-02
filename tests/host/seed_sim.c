// seed_sim: runs the Seed's drop-in receiver (slp_receiver.c) on a PC serial port, so the real
// ESP32 firmware can be tested against the exact code the Seed will compile (only the Daisy UART
// glue differs). Windows only. Everything it reports as audio state is SIMULATED.
//
//   build/host/seed_sim.exe COM7 [seconds]
//
// Prints one line per state change the ESP32 sends, plus a summary at the end.
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "slp_receiver.h"

static HANDLE s_port;
static LARGE_INTEGER s_freq, s_t0;
static uint32_t s_params, s_frames_sent;
static uint8_t s_model_buf[7484];
static uint8_t s_active_kind = SLP_MODEL_BUILTIN, s_active_builtin = 1;
static uint32_t s_active_hash;
static bool s_failed;

static uint32_t now_ms(void *ctx) {
    (void)ctx;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000 / s_freq.QuadPart);
}

static void port_send(void *ctx, const uint8_t *b, size_t n) {
    (void)ctx;
    DWORD w;
    WriteFile(s_port, b, (DWORD)n, &w, NULL);
    s_frames_sent++;
}

static void on_param(void *c, uint16_t id, int16_t v) { (void)c; (void)id; (void)v; s_params++; }
static void on_chain(void *c, const uint8_t *fx, uint8_t len) {
    (void)c;
    printf("%8.3f chain:", now_ms(NULL) / 1000.0);
    for (int i = 0; i < len; i++) printf(" %u", fx[i]);
    printf(" (then cab)\n");
}
static void on_master(void *c, uint8_t v, bool m) { (void)c; printf("%8.3f master %u%s\n", now_ms(NULL) / 1000.0, v, m ? " MUTED" : ""); }
static void on_select(void *c, const slp_select_model_t *m, const uint8_t *w) {
    (void)c;
    s_active_kind = m->kind;
    s_active_builtin = m->builtin_id;
    s_active_hash = m->hash;
    // A real Seed would copy w into its running weights buffer here, with audio stopped.
    s_failed = m->kind == SLP_MODEL_UPLOADED && !w;
    printf("%8.3f select model kind %u builtin %u hash %08lx%s\n", now_ms(NULL) / 1000.0, m->kind, m->builtin_id,
           (unsigned long)m->hash, m->kind == SLP_MODEL_UPLOADED ? (w ? " (weights held, CRC ok)" : " (NOT held)") : "");
}
static void on_link(void *c, bool up) { (void)c; printf("%8.3f link %s\n", now_ms(NULL) / 1000.0, up ? "UP" : "DOWN"); }

int main(int argc, char **argv) {
    const char *port = argc > 1 ? argv[1] : "COM7";
    const double seconds = argc > 2 ? atof(argv[2]) : 60;
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    s_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s_port == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "can't open %s\n", port);
        return 2;
    }
    DCB dcb = { .DCBlength = sizeof(dcb) };
    GetCommState(s_port, &dcb);
    dcb.BaudRate = 1000000;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_DISABLE; // don't reset the ESP32 through its auto-reset circuit
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    SetCommState(s_port, &dcb);
    COMMTIMEOUTS to = { .ReadIntervalTimeout = MAXDWORD, .ReadTotalTimeoutConstant = 2 };
    SetCommTimeouts(s_port, &to);
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_t0);

    static const slp_builtin_t builtins[3] = { { 1, "Fender Twin65" }, { 2, "Vox AC30 Chimey" }, { 3, "Marshall JCM800 G5" } };
    const slp_rx_config_t cfg = {
        .send = port_send, .now_ms = now_ms, .boot_id = GetTickCount() ^ 0xA5A5A5A5u, .fw_version = "seed-sim",
        .sample_rate_hz = 48000, .block_size = 48, .builtins = builtins, .builtin_count = 3,
        .role = SLP_ROLE_MOCK_SEED, // simulated: the ESP32 labels everything it reports MOCK
        .model_buf = s_model_buf, .model_buf_size = sizeof(s_model_buf), .on_param = on_param,
        .on_chain = on_chain, .on_master = on_master, .on_select_model = on_select, .on_link = on_link,
    };
    static slp_receiver_t r;
    slp_rx_init(&r, &cfg);
    uint32_t next_status = 0, next_meters = 0;
    printf("seed_sim on %s: slp_receiver.c, SIMULATED audio\n", port);
    while (now_ms(NULL) < seconds * 1000) {
        uint8_t buf[512];
        DWORD n = 0;
        if (ReadFile(s_port, buf, sizeof(buf), &n, NULL) && n) slp_rx_feed(&r, buf, n);
        slp_rx_poll(&r);
        const uint32_t now = now_ms(NULL);
        if (slp_rx_link_up(&r) && now >= next_status) {
            const slp_status_t st = { 550, 560, 0, s_active_kind, s_active_builtin, s_active_hash,
                                      (uint8_t)(SLP_STATUS_AUDIO_RUNNING | (s_failed ? SLP_STATUS_MODEL_FAILED : 0)), 0 };
            slp_rx_send_status(&r, &st);
            next_status = now + 500;
        }
        if (slp_rx_link_up(&r) && now >= next_meters) {
            const slp_meters_t m = { -2400, -1800, 0 };
            slp_rx_send_meters(&r, &m);
            next_meters = now + 33;
        }
    }
    printf("summary: %lu frames received, %lu dropped, %lu param updates, %lu frames sent, last snapshot applied %u\n",
           (unsigned long)r.dec.frames_ok, (unsigned long)slp_decoder_errors(&r.dec), (unsigned long)s_params,
           (unsigned long)s_frames_sent, r.applied_snapshot);
    CloseHandle(s_port);
    return 0;
}
#else
int main(void) { return 0; }
#endif
