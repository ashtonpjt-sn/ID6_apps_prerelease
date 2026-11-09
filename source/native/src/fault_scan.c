/* Fault Scanner worker.
 *   --scan          read stored fault codes from every control unit (read only)
 *   --show PAGE     show a page of the last saved scan (no vehicle traffic)
 *   --clear all|XX  clear fault memory in every unit with faults, or in unit XX,
 *                   after confirming the car is stationary; then scan again
 * Results are saved with --file so pages can be shown without asking the car. */
#define _POSIX_C_SOURCE 200809L
#include "diagnostic_lease.h"
#include "gauge_core.h"
#include "hsfz.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { M_STATUS = 50, M_LINE = 51, LINES = 8, M_CLEAR_RESULT = 61 };
enum { MAX_UNITS = 64, MAX_CODES = 256, DTC_MASK = 0x0c };

typedef struct { unsigned unit, code, status; } fault;
typedef struct {
    int answered;
    unsigned units[MAX_UNITS];
    unsigned unit_count;
    fault faults[MAX_CODES];
    unsigned fault_count;
    int truncated;
} scan_result;

static const struct { unsigned address; const char *name; } unit_names[] = {
    {0x00, "Gateway (ZGM)"}, {0x01, "Airbag (ACSM)"}, {0x12, "Engine (DME)"},
    {0x18, "Transmission (EGS)"}, {0x1c, "Chassis sensors (ICM)"}, {0x29, "Stability control (DSC)"},
    {0x30, "Steering (EPS)"}, {0x40, "Body (BDC/FEM)"}, {0x60, "Instrument cluster (KOMBI)"},
    {0x63, "Head unit"}, {0x78, "Climate (IHKA)"},
};

static void unit_name(unsigned address, char *out, size_t size) {
    unsigned i;
    for (i = 0; i < sizeof unit_names / sizeof unit_names[0]; ++i)
        if (unit_names[i].address == address) {
            snprintf(out, size, "%s %02X", unit_names[i].name, address);
            return;
        }
    snprintf(out, size, "Control unit %02X", address);
}

static const char *status_text(unsigned status) {
    if (status & 0x01) return "active now";
    if (status & 0x08) return "stored";
    if (status & 0x04) return "pending";
    return "logged";
}

/* ---- Scan --------------------------------------------------------------- */
static void add_unit(scan_result *r, unsigned unit) {
    unsigned i;
    for (i = 0; i < r->unit_count; ++i) if (r->units[i] == unit) return;
    if (r->unit_count < MAX_UNITS) r->units[r->unit_count++] = unit;
}

static void collect(void *context, unsigned source, const unsigned char *uds, unsigned size) {
    scan_result *r = (scan_result *)context;
    unsigned i;
    add_unit(r, source);
    if (size < 3 || uds[0] != 0x59 || uds[1] != 0x02) return;
    for (i = 3; i + 4 <= size; i += 4) {
        if (!(uds[i+3] & DTC_MASK)) continue;
        if (r->fault_count >= MAX_CODES) { r->truncated = 1; return; }
        r->faults[r->fault_count].unit = source;
        r->faults[r->fault_count].code = ((unsigned)uds[i] << 16) | ((unsigned)uds[i+1] << 8) | uds[i+2];
        r->faults[r->fault_count].status = uds[i+3];
        ++r->fault_count;
    }
}

static int compare_faults(const void *a, const void *b) {
    const fault *x = (const fault *)a, *y = (const fault *)b;
    if (x->unit != y->unit) return x->unit < y->unit ? -1 : 1;
    return x->code < y->code ? -1 : x->code > y->code;
}

static int scan(int fd, scan_result *r) {
    const unsigned char request[] = {0x19, 0x02, DTC_MASK};
    int n;
    memset(r, 0, sizeof *r);
    n = hsfz_functional(fd, request, sizeof request, 1500, 12000, collect, r);
    if (n < 0) return -1;
    r->answered = 1;
    qsort(r->faults, r->fault_count, sizeof r->faults[0], compare_faults);
    return 0;
}

static int save(const scan_result *r, const char *path) {
    char temporary[256];
    FILE *f;
    unsigned i;
    if (!path || snprintf(temporary, sizeof temporary, "%s.new", path) >= (int)sizeof temporary) return 0;
    f = fopen(temporary, "w");
    if (!f) return 0;
    fprintf(f, "scan %u %u %d\n", r->unit_count, r->fault_count, r->truncated);
    for (i = 0; i < r->unit_count; ++i) fprintf(f, "unit %02X\n", r->units[i]);
    for (i = 0; i < r->fault_count; ++i)
        fprintf(f, "code %02X %06X %02X\n", r->faults[i].unit, r->faults[i].code, r->faults[i].status);
    if (fclose(f) != 0) { remove(temporary); return 0; }
    return rename(temporary, path) == 0;
}

static int load(scan_result *r, const char *path) {
    FILE *f = path ? fopen(path, "r") : 0;
    char line[96];
    memset(r, 0, sizeof *r);
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned a, b, c;
        int t;
        if (sscanf(line, "scan %u %u %d", &a, &b, &t) == 3) { r->answered = 1; r->truncated = t; }
        else if (sscanf(line, "unit %x", &a) == 1 && a <= 0xff) add_unit(r, a);
        else if (sscanf(line, "code %x %x %x", &a, &b, &c) == 3 && a <= 0xff &&
                 b <= 0xffffff && c <= 0xff && r->fault_count < MAX_CODES) {
            r->faults[r->fault_count].unit = a;
            r->faults[r->fault_count].code = b;
            r->faults[r->fault_count].status = c;
            ++r->fault_count;
        }
    }
    fclose(f);
    return r->answered;
}

/* ---- Pages -------------------------------------------------------------- */
/* Line k of the listing: a heading per unit, then one line per code. */
static int listing_line(const scan_result *r, unsigned k, char *out, size_t size) {
    unsigned i = 0, line = 0;
    while (i < r->fault_count) {
        unsigned unit = r->faults[i].unit, j = i, count = 0;
        char name[64];
        while (j < r->fault_count && r->faults[j].unit == unit) { ++j; ++count; }
        if (line++ == k) {
            unit_name(unit, name, sizeof name);
            snprintf(out, size, "%s:  %u %s", name, count, count == 1 ? "fault" : "faults");
            return 1;
        }
        for (; i < j; ++i)
            if (line++ == k) {
                snprintf(out, size, "      %06X   %s", r->faults[i].code, status_text(r->faults[i].status));
                return 1;
            }
    }
    return 0;
}

static unsigned listing_size(const scan_result *r) {
    unsigned i, lines = r->fault_count;
    for (i = 0; i < r->fault_count; ++i)
        if (!i || r->faults[i].unit != r->faults[i-1].unit) ++lines;
    return lines;
}

static unsigned units_with_faults(const scan_result *r) {
    unsigned i, n = 0;
    for (i = 0; i < r->fault_count; ++i)
        if (!i || r->faults[i].unit != r->faults[i-1].unit) ++n;
    return n;
}

static void show(const scan_result *r, unsigned page) {
    unsigned pages, lines = listing_size(r), i;
    char text[160];
    pages = lines ? (lines + LINES - 1) / LINES : 1;
    if (page >= pages) page = pages - 1;
    hsfz_value_line(1, (long)page); /* the plugin pages from here */
    if (!r->answered) {
        hsfz_text(M_STATUS, "No scan yet. Press OPTIONS, then Scan all modules.");
    } else if (!r->unit_count) {
        hsfz_text(M_STATUS, "No control unit answered. Is the ignition on?");
    } else if (!r->fault_count) {
        hsfz_text(M_STATUS, "No stored faults in %u control units", r->unit_count);
    } else {
        hsfz_text(M_STATUS, "%u %s in %u of %u control units%s   Page %u/%u", r->fault_count,
                  r->fault_count == 1 ? "fault" : "faults", units_with_faults(r), r->unit_count,
                  r->truncated ? " (first 256)" : "", page + 1, pages);
    }
    for (i = 0; i < LINES; ++i) {
        if (!listing_line(r, page * LINES + i, text, sizeof text)) text[0] = 0;
        hsfz_text(M_LINE + i, "%s", text);
    }
}

/* ---- Clear -------------------------------------------------------------- */
static int session_control(int fd, unsigned unit, unsigned type) {
    unsigned char request[2], prefix[2], reply[16];
    unsigned negative;
    request[0] = 0x10; request[1] = (unsigned char)type;
    prefix[0] = 0x50; prefix[1] = (unsigned char)type;
    return hsfz_exchange(fd, unit, request, 2, prefix, 2, reply, sizeof reply, 3000, &negative);
}

/* Returns 1 cleared, 0 refused (reason in why), -1 transport failure. */
static int clear_unit(int fd, unsigned unit, char *why, size_t size) {
    const unsigned char request[] = {0x14, 0xff, 0xff, 0xff}, prefix[] = {0x54};
    unsigned char reply[16];
    unsigned negative = 0;
    int n = hsfz_exchange(fd, unit, request, 4, prefix, 1, reply, sizeof reply, 8000, &negative);
    if (n == -1) n = hsfz_exchange(fd, unit, request, 4, prefix, 1, reply, sizeof reply, 8000, &negative);
    if (n == -2 && (negative == 0x22 || negative == 0x31 || negative == 0x33 ||
                    negative == 0x7e || negative == 0x7f)) {
        /* Some units only clear in the extended diagnostic session. */
        unsigned first = negative;
        int session = session_control(fd, unit, 0x03);
        if (session == -1) return -1;
        if (session > 0) {
            n = hsfz_exchange(fd, unit, request, 4, prefix, 1, reply, sizeof reply, 8000, &negative);
            if (n == -1) return -1;
            if (session_control(fd, unit, 0x01) == -1) return -1;
        }
        if (n < 0) negative = first;
    }
    if (n >= 1) return 1;
    if (n == -1) { snprintf(why, size, "no answer"); return 0; }
    if (negative == 0x22) snprintf(why, size, "refused now: try with the engine off, ignition on");
    else snprintf(why, size, "refused (NRC %02X)", negative);
    return 0;
}

typedef struct { unsigned ok[MAX_UNITS]; unsigned count; } acknowledged;
static void collect_clear(void *context, unsigned source, const unsigned char *uds, unsigned size) {
    acknowledged *a = (acknowledged *)context;
    if (size >= 1 && uds[0] == 0x54 && a->count < MAX_UNITS) a->ok[a->count++] = source;
}

/* The car must report 0 km/h before anything is cleared. */
static int stationary(int fd) {
    unsigned char data[4];
    int n = hsfz_obd(fd, 0x12, 0x0d, data, sizeof data, 1000);
    if (n == -1) return -1;
    return n == 1 && data[0] == 0;
}

static int clear(int fd, const char *target, const scan_result *last) {
    char why[96], name[64], failed[64 + 96 + 4] = "";
    unsigned units[MAX_UNITS], count = 0, cleared = 0, i, j;
    int single = strcmp(target, "all") != 0, rc;
    rc = stationary(fd);
    if (rc < 0) return -1;
    if (!rc) {
        hsfz_text(M_CLEAR_RESULT, "Could not confirm the car is stopped. Nothing was cleared.");
        return 0;
    }
    if (single) {
        units[count++] = hsfz_address(target);
    } else {
        for (i = 0; i < last->fault_count; ++i)
            if (!i || last->faults[i].unit != last->faults[i-1].unit) units[count++] = last->faults[i].unit;
        if (!count) {
            hsfz_text(M_CLEAR_RESULT, last->answered ? "Nothing to clear: the last scan found no faults."
                                                     : "Scan first, then clear.");
            return 0;
        }
    }
    if (!single) {
        /* Clear every unit at once, as a workshop tester does: some units only
         * accept the functional request while the engine is off. */
        const unsigned char request[] = {0x14, 0xff, 0xff, 0xff};
        acknowledged a;
        memset(&a, 0, sizeof a);
        hsfz_text(M_CLEAR_RESULT, "Clearing all control units...");
        if (hsfz_functional(fd, request, sizeof request, 3000, 10000, collect_clear, &a) < 0) return -1;
        for (i = 0; i < count; ++i) {
            for (j = 0; j < a.count; ++j) if (a.ok[j] == units[i]) break;
            if (j < a.count) { ++cleared; units[i] = 0x100; }
        }
    }
    for (i = 0; i < count; ++i) {
        if (units[i] > 0xff) continue;
        unit_name(units[i], name, sizeof name);
        hsfz_text(M_CLEAR_RESULT, "Clearing %s...", name);
        rc = clear_unit(fd, units[i], why, sizeof why);
        if (rc < 0) return -1;
        if (rc) ++cleared;
        else if (!failed[0]) snprintf(failed, sizeof failed, "%s: %s", name, why);
    }
    if (failed[0]) hsfz_text(M_CLEAR_RESULT, "Cleared %u of %u. Not cleared: %s", cleared, count, failed);
    else hsfz_text(M_CLEAR_RESULT, "Cleared %u of %u. Faults that return at once are still present.",
                   cleared, count);
    return 1;
}

int main(int argc, char **argv) {
    const char *host = 0, *file = 0, *target = 0;
    unsigned port = HSFZ_PORT, page = 0;
    int mode = 0, lock = -1, fd = -1, i, rc = 0;
    scan_result result;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc && !host) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = hsfz_number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--scan") && !mode) mode = 1;
        else if (!strcmp(argv[i], "--show") && i + 1 < argc && !mode) { mode = 2; page = hsfz_number(argv[++i], 0, 999); }
        else if (!strcmp(argv[i], "--clear") && i + 1 < argc && !mode) { mode = 3; target = argv[++i]; }
        else break;
    }
    if (i != argc || !mode || (mode != 2 && !host)) {
        fprintf(stderr, "Usage: %s --host IPv4 [--port 6801] [--file PATH] (--scan | --show PAGE | --clear all|XX)\n", argv[0]);
        return 2;
    }
    if (mode == 3 && strcmp(target, "all")) (void)hsfz_address(target);
    hsfz_signals();
    if (mode == 2) {
        load(&result, file);
        show(&result, page);
        return 0;
    }
    lock = reserve_gateway();
    if (lock < 0) {
        hsfz_text(mode == 3 ? M_CLEAR_RESULT : M_STATUS, "Diagnostics busy. Wait a moment and try again.");
        return 0;
    }
    fd = hsfz_connect(host, port);
    if (fd < 0) {
        hsfz_text(mode == 3 ? M_CLEAR_RESULT : M_STATUS, "Diagnostic gateway unavailable");
        close(lock);
        return 0;
    }
    if (mode == 3) {
        load(&result, file);
        rc = clear(fd, target, &result);
        if (rc < 0) hsfz_text(M_CLEAR_RESULT, "Connection lost while clearing. Scan again to check.");
    }
    if (rc >= 0 && (mode == 1 || rc > 0)) {
        hsfz_text(M_STATUS, "Scanning all control units...");
        for (i = 0; i < LINES; ++i) hsfz_text(M_LINE + i, "%s", "");
        if (scan(fd, &result) < 0) {
            hsfz_text(M_STATUS, "Connection lost during the scan. Try again.");
        } else {
            if (!save(&result, file) && file) fprintf(stderr, "could not save %s\n", file);
            show(&result, 0);
        }
    }
    close(fd);
    close(lock);
    return 0;
}
