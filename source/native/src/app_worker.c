#define _POSIX_C_SOURCE 200809L
#include "app_worker.h"
#include "gauge_core.h"
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_text(unsigned model, const char *format, ...) {
    char line[400];
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    /* One update per line: never let a value break the protocol. */
    for (n = 0; line[n]; ++n) if (line[n] == '\n' || line[n] == '\r') line[n] = ' ';
    if (printf("T%u %s\n", model, line) < 0 || fflush(stdout) != 0) diag_running = 0;
}

static char shown_text[256][200];
static unsigned char shown_valid[256];
static unsigned shown_image[256];
static unsigned char shown_image_valid[256];

void app_text_once(unsigned model, const char *format, ...) {
    char line[200];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (model < 256 && shown_valid[model] && !strcmp(shown_text[model], line)) return;
    if (model < 256) {
        strcpy(shown_text[model], line);
        shown_valid[model] = 1;
    }
    app_text(model, "%s", line);
}

void app_image(unsigned model, unsigned image) {
    if (printf("I%u %u\n", model, image) < 0 || fflush(stdout) != 0) diag_running = 0;
}

void app_image_once(unsigned model, unsigned image) {
    if (model < 256 && shown_image_valid[model] && shown_image[model] == image) return;
    if (model < 256) {
        shown_image[model] = image;
        shown_image_valid[model] = 1;
    }
    app_image(model, image);
}

void app_value_line(unsigned key, long number) {
    if (printf("V%u %ld\n", key, number) < 0 || fflush(stdout) != 0) diag_running = 0;
}

void app_pause(unsigned ms) {
    uint64_t end = bd_mono_ms() + ms;
    while (diag_running) {
        uint64_t now = bd_mono_ms();
        if (now >= end) break;
        poll(0, 0, (int)(end - now));
    }
}

unsigned app_number(const char *s, unsigned lo, unsigned hi) {
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n < lo || n > hi) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}

unsigned app_address(const char *s) {
    char *end;
    unsigned long n = strtoul(s, &end, 16);
    if (!*s || *end || n > 0xff) { fprintf(stderr, "Invalid control unit address: %s\n", s); exit(2); }
    return (unsigned)n;
}

int app_link_option(int argc, char **argv, int *i, struct diag_link_cfg *cfg) {
    const char *name = argv[*i], *value = *i + 1 < argc ? argv[*i + 1] : 0;
    if (!value) return 0;
    if (!strcmp(name, "--host") && !cfg->host) cfg->host = value;
    else if (!strcmp(name, "--port")) cfg->port = app_number(value, 1, 65535);
    else if (!strcmp(name, "--transport") && !strcmp(value, "auto")) cfg->transport = DIAG_AUTO;
    else if (!strcmp(name, "--transport") && !strcmp(value, "hsfz")) cfg->transport = DIAG_HSFZ;
    else if (!strcmp(name, "--transport") && !strcmp(value, "most")) cfg->transport = DIAG_MOST;
    else return 0;
    ++*i;
    return 1;
}

/* A read with a fixed request and echo. On MOST a failed request (-3) keeps the link, so it counts as
   unavailable for now. */
static int read_data(struct diag_link *link, unsigned ecu, const unsigned char *request, unsigned n,
                     unsigned char *data, unsigned size, unsigned timeout_ms) {
    unsigned char prefix[3], reply[128];
    unsigned negative;
    int got;
    memcpy(prefix, request, n);
    prefix[0] = (unsigned char)(request[0] + 0x40);
    got = diag_link_request(link, ecu, request, n, prefix, n, reply, sizeof reply, timeout_ms, &negative);
    if (got == -1) return -1;
    if (got < (int)n) return -2;
    got -= (int)n;
    if ((unsigned)got > size) got = (int)size;
    memcpy(data, reply + n, (size_t)got);
    return got;
}

int app_obd(struct diag_link *link, unsigned ecu, unsigned pid, unsigned char *data, unsigned size,
            unsigned timeout_ms) {
    unsigned char request[2];
    request[0] = 0x01; request[1] = (unsigned char)pid;
    return read_data(link, ecu, request, 2, data, size, timeout_ms);
}

int app_did(struct diag_link *link, unsigned ecu, unsigned did, unsigned char *data, unsigned size,
            unsigned timeout_ms) {
    unsigned char request[3];
    request[0] = 0x22; request[1] = (unsigned char)(did >> 8); request[2] = (unsigned char)did;
    return read_data(link, ecu, request, 3, data, size, timeout_ms);
}

static int read_candidate(struct diag_link *link, const app_candidate *c, double *value) {
    unsigned char data[64];
    double raw = 0;
    unsigned i;
    int n = c->obd ? app_obd(link, c->ecu, c->id, data, sizeof data, 300)
                   : app_did(link, c->ecu, c->id, data, sizeof data, 300);
    if (n == -1) return -1;
    if (n < 0 || !c->bytes || c->bytes > 4 || (unsigned)n < c->at + c->bytes) return 0;
    for (i = 0; i < c->bytes; ++i) raw = raw * 256 + data[c->at + i];
    if (c->is_signed && raw >= ldexp(1.0, 8 * (int)c->bytes - 1)) raw -= ldexp(1.0, 8 * (int)c->bytes);
    *value = raw * c->factor + c->offset;
    return *value >= c->min && *value <= c->max;
}

int app_value(struct diag_link *link, const app_candidate *list, unsigned count, unsigned *pick,
              double *value) {
    unsigned k;
    for (k = 0; k < count; ++k) {
        unsigned i = (*pick + k) % count;
        int rc = read_candidate(link, &list[i], value);
        if (rc < 0) return -1;
        if (rc) { *pick = i; return 1; }
    }
    return 0;
}

void app_link_done(struct diag_link *link, int ok) {
    if (!ok) diag_link_drop(link);
    else (void)diag_link_should_yield(link, bd_mono_ms());
}
