/* BoostDeck "More vitals" page: boostdeck-collector-qnx --vitals --host IP.
 * Read only. Candidate sources come from public OBD definitions and the
 * idrive7-apps M Vitals list; each is shown only when its value is plausible.
 * Validate them against a logger before relying on them (docs/SIGNALS.md). */
#define _POSIX_C_SOURCE 200809L
#include "gauge_core.h"
#include "hsfz.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { M_OIL = 120, M_LPFP, M_BATTERY, M_TRIMS, M_ETHANOL, M_LOAD, M_MISFIRE_A, M_MISFIRE_B };

static const hsfz_candidate oil[] = {
    {0x12, 1, 0x5c, 0, 1, 0, 1, -40, -40, 170},
    {0x12, 0, 0xf45c, 0, 1, 0, 1, -40, -40, 170},
};
static const hsfz_candidate lpfp[] = { /* bar */
    {0x12, 0, 0xf40a, 0, 1, 0, 0.03, 0, 0.5, 8},
    {0x12, 0, 0xf422, 0, 2, 0, 0.00079, 0, 0.5, 15},
    {0x12, 1, 0x0a, 0, 1, 0, 0.03, 0, 0.5, 8},
    {0x12, 1, 0x22, 0, 2, 0, 0.00079, 0, 0.5, 15},
};
static const hsfz_candidate battery[] = { /* volts reported by the DME */
    {0x12, 1, 0x42, 0, 2, 0, 0.001, 0, 6, 18},
};
static const hsfz_candidate short_trim[] = {{0x12, 1, 0x06, 0, 1, 0, 100.0 / 128, -100, -100, 99.3}};
static const hsfz_candidate long_trim[] = {{0x12, 1, 0x07, 0, 1, 0, 100.0 / 128, -100, -100, 99.3}};
static const hsfz_candidate ethanol[] = {{0x12, 1, 0x52, 0, 1, 0, 100.0 / 255, 0, 0, 100}};
static const hsfz_candidate load[] = {{0x12, 1, 0x04, 0, 1, 0, 100.0 / 255, 0, 0, 100}};

typedef struct {
    const hsfz_candidate *list;
    unsigned count, pick;
    double value;
    uint64_t at;
} source;

static int refresh(int fd, source *s, uint64_t now) {
    double v;
    int rc = hsfz_value(fd, s->list, s->count, &s->pick, &v);
    if (rc > 0) { s->value = v; s->at = now; }
    return rc;
}

static int fresh(const source *s, uint64_t now) { return s->at && now - s->at < 4000; }

/* OBD service 06 misfire monitor: MID A2..A7 = cylinders 1..6, TID 0C =
 * misfires in the current driving cycle. Returns count, -2 unavailable, -1 lost. */
static int misfires(int fd, unsigned cylinder) {
    unsigned char request[2], prefix[2], reply[128];
    unsigned negative;
    int n, i;
    request[0] = 0x06; request[1] = (unsigned char)(0xa1 + cylinder);
    prefix[0] = 0x46; prefix[1] = request[1];
    n = hsfz_exchange(fd, 0x12, request, 2, prefix, 2, reply, sizeof reply, 400, &negative);
    if (n == -1) return -1;
    for (i = 1; n > 0 && i + 9 <= n; i += 9)
        if (reply[i] == request[1] && reply[i+1] == 0x0c) return (reply[i+3] << 8) | reply[i+4];
    return -2;
}

static void misfire_line(unsigned model, const int *counts, unsigned first) {
    char parts[3][16];
    unsigned i;
    for (i = 0; i < 3; ++i) {
        if (counts[first + i] >= 0) snprintf(parts[i], sizeof parts[i], "%d", counts[first + i]);
        else snprintf(parts[i], sizeof parts[i], "--");
    }
    hsfz_text_once(model, "Misfires this drive   cyl %u: %s   cyl %u: %s   cyl %u: %s",
                   first + 1, parts[0], first + 2, parts[1], first + 3, parts[2]);
}

int vitals_main(int argc, char **argv) {
    const char *host = 0;
    unsigned port = HSFZ_PORT, cycles = 0, count = 0, i;
    pid_t parent = getppid();
    hsfz_session session;
    source sources[7] = {
        {oil, 2, 0, 0, 0}, {lpfp, 4, 0, 0, 0}, {battery, 1, 0, 0, 0}, {short_trim, 1, 0, 0, 0},
        {long_trim, 1, 0, 0, 0}, {ethanol, 1, 0, 0, 0}, {load, 1, 0, 0, 0}};
    int counts[6] = {-2, -2, -2, -2, -2, -2};
    uint64_t misfire_due = 0;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--vitals")) continue;
        if (!strcmp(argv[i], "--host") && i + 1 < (unsigned)argc && !host) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < (unsigned)argc) port = hsfz_number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < (unsigned)argc) cycles = hsfz_number(argv[++i], 1, 100000);
        else { fprintf(stderr, "Usage: %s --vitals --host IPv4 [--port 6801] [--cycles N]\n", argv[0]); return 2; }
    }
    if (!host) return 2;
    hsfz_signals();
    hsfz_session_init(&session, host, port);
    while (hsfz_running && getppid() == parent && (!cycles || count < cycles)) {
        uint64_t now = bd_mono_ms();
        int fd = hsfz_session_fd(&session), ok = 1;
        if (fd >= 0) {
            for (i = 0; ok && i < 7; ++i) if (refresh(fd, &sources[i], now) < 0) ok = 0;
            if (ok && now >= misfire_due) {
                for (i = 0; ok && i < 6; ++i) {
                    int n = misfires(fd, i + 1);
                    if (n == -1) ok = 0;
                    else counts[i] = n;
                }
                misfire_due = now + 5000;
            }
            hsfz_session_done(&session, ok);
        }
        if (fresh(&sources[0], now))
            hsfz_text_once(M_OIL, "Engine oil   %.0f\260F   (%.0f\260C)", sources[0].value * 9 / 5 + 32, sources[0].value);
        else hsfz_text_once(M_OIL, "Engine oil   --");
        if (fresh(&sources[1], now)) hsfz_text_once(M_LPFP, "Low-pressure fuel   %.0f psi", sources[1].value * 14.5038);
        else hsfz_text_once(M_LPFP, "Low-pressure fuel   --");
        if (fresh(&sources[2], now)) hsfz_text_once(M_BATTERY, "Battery   %.1f V", sources[2].value);
        else hsfz_text_once(M_BATTERY, "Battery   --");
        if (fresh(&sources[3], now) && fresh(&sources[4], now))
            hsfz_text_once(M_TRIMS, "Fuel trims   short %+.1f%%   long %+.1f%%", sources[3].value, sources[4].value);
        else hsfz_text_once(M_TRIMS, "Fuel trims   --");
        if (fresh(&sources[5], now)) hsfz_text_once(M_ETHANOL, "Ethanol content   %.0f%%", sources[5].value);
        else hsfz_text_once(M_ETHANOL, "Ethanol content   --");
        if (fresh(&sources[6], now)) hsfz_text_once(M_LOAD, "Engine load   %.0f%%", sources[6].value);
        else hsfz_text_once(M_LOAD, "Engine load   --");
        misfire_line(M_MISFIRE_A, counts, 0);
        misfire_line(M_MISFIRE_B, counts, 3);
        ++count;
        if (!cycles || count < cycles) {
            uint64_t elapsed = bd_mono_ms() - now;
            hsfz_pause(elapsed < 500 ? (unsigned)(500 - elapsed) : 20);
        }
    }
    hsfz_session_close(&session);
    return 0;
}
