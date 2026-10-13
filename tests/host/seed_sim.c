// seed_sim: runs the Seed's drop-in receiver (slp_receiver.c) on a PC serial port, so the real
// ESP32 firmware can be tested against the exact code the Seed will compile (only the Daisy UART
// glue differs). Windows, macOS and Linux. Everything it reports as audio state is SIMULATED.
//
//   build/host/seed_sim.exe COM7 [seconds]
//   build/host/seed_sim /dev/cu.usbmodemXXXX [seconds]
//
// Prints one line per state change the ESP32 sends, plus a summary at the end.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slp_receiver.h"

// ---------------------------------------------------------------------------------------------
// Serial port and clock, per platform
#ifdef _WIN32
#include <windows.h>

static HANDLE s_port;
static LARGE_INTEGER s_freq, s_t0;

static uint32_t now_ms(void *ctx) {
    (void)ctx;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000 / s_freq.QuadPart);
}

static bool port_open(const char *port) {
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    s_port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s_port == INVALID_HANDLE_VALUE) return false;
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
    return true;
}

static void port_write(const uint8_t *b, size_t n) {
    DWORD w;
    WriteFile(s_port, b, (DWORD)n, &w, NULL);
}

static size_t port_read(uint8_t *buf, size_t cap) {
    DWORD n = 0;
    return ReadFile(s_port, buf, (DWORD)cap, &n, NULL) ? n : 0;
}

static void port_close(void) { CloseHandle(s_port); }

static uint32_t boot_seed(void) { return GetTickCount(); }

#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h> // IOSSIOSPEED: termios has no B1000000 on macOS
#endif

static int s_fd = -1;
static struct timespec s_t0;

static uint32_t now_ms(void *ctx) {
    (void)ctx;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - s_t0.tv_sec) * 1000 + (t.tv_nsec - s_t0.tv_nsec) / 1000000);
}

static bool port_open(const char *port) {
    s_fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s_fd < 0) return false;
    struct termios tio;
    if (tcgetattr(s_fd, &tio) != 0) return false;
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    // 8N1, no flow control, and no HUPCL so closing the port doesn't toggle DTR.
    tio.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS | HUPCL);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
#ifdef __APPLE__
    if (tcsetattr(s_fd, TCSANOW, &tio) != 0) return false;
    speed_t speed = 1000000;
    if (ioctl(s_fd, IOSSIOSPEED, &speed) != 0) return false;
#else
    cfsetispeed(&tio, B1000000);
    cfsetospeed(&tio, B1000000);
    if (tcsetattr(s_fd, TCSANOW, &tio) != 0) return false;
#endif
    // Don't reset the ESP32 through its auto-reset circuit: hold DTR and RTS inactive.
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(s_fd, TIOCMBIC, &lines);
    tcflush(s_fd, TCIOFLUSH);
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    return true;
}

static void port_write(const uint8_t *b, size_t n) {
    while (n) {
        const ssize_t w = write(s_fd, b, n);
        if (w > 0) {
            b += w;
            n -= (size_t)w;
        } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
            return;
        } else {
            struct pollfd p = { .fd = s_fd, .events = POLLOUT };
            poll(&p, 1, 2);
        }
    }
}

static size_t port_read(uint8_t *buf, size_t cap) {
    struct pollfd p = { .fd = s_fd, .events = POLLIN };
    if (poll(&p, 1, 2) <= 0) return 0;
    const ssize_t n = read(s_fd, buf, cap);
    return n > 0 ? (size_t)n : 0;
}

static void port_close(void) { close(s_fd); }

static uint32_t boot_seed(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint32_t)t.tv_sec ^ (uint32_t)t.tv_nsec;
}
#endif

// ---------------------------------------------------------------------------------------------
// The simulated Seed
static uint32_t s_params, s_frames_sent;
static uint8_t s_model_buf[7484];
static uint8_t s_active_kind = SLP_MODEL_BUILTIN, s_active_builtin = 1;
static uint32_t s_active_hash;
static bool s_failed;

static void port_send(void *ctx, const uint8_t *b, size_t n) {
    (void)ctx;
    port_write(b, n);
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
#ifdef _WIN32
    const char *port = argc > 1 ? argv[1] : "COM7";
#else
    if (argc < 2) {
        fprintf(stderr, "usage: seed_sim /dev/cu.usbmodemXXXX [seconds]\n");
        return 2;
    }
    const char *port = argv[1];
#endif
    const double seconds = argc > 2 ? atof(argv[2]) : 60;
    if (!port_open(port)) {
        fprintf(stderr, "can't open %s at 1000000 baud\n", port);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    static const slp_builtin_t builtins[3] = { { 1, "Fender Twin65" }, { 2, "Vox AC30 Chimey" }, { 3, "Marshall JCM800 G5" } };
    const slp_rx_config_t cfg = {
        .send = port_send, .now_ms = now_ms, .boot_id = boot_seed() ^ 0xA5A5A5A5u, .fw_version = "seed-sim",
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
        const size_t n = port_read(buf, sizeof(buf));
        if (n) slp_rx_feed(&r, buf, n);
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
    port_close();
    return 0;
}
