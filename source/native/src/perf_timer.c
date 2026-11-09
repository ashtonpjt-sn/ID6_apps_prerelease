/* Performance Timer worker: 0-60 mph, quarter mile, 60-100 and 60-130 mph and
 * 60-0 mph braking from the engine's OBD speed. Read only. Runs are detected
 * automatically; the best results are kept in a small state file. */
#define _POSIX_C_SOURCE 200809L
#include "drive_core.h"
#include "gauge_core.h"
#include "hsfz.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { M_STATUS = 50, M_FIRST_RESULT = 51, M_NOTE = 56 };

static const char *const names[PERF_COUNT] = {
    "0-60 mph", "1/4 mile", "60-100 mph", "60-130 mph", "60-0 mph braking"};

static double mph(double kph) { return kph / 1.609344; }

static void describe(int which, const perf_result *r, char *out, size_t size) {
    if (!r->valid) snprintf(out, size, "--");
    else if (which == PERF_QUARTER) snprintf(out, size, "%.1f s @ %.0f mph", r->seconds, mph(r->trap_kph));
    else if (which == PERF_BRAKE) snprintf(out, size, "%.0f ft (%.2f g)", r->meters * 3.28084, r->decel_g);
    else snprintf(out, size, "%.1f s", r->seconds);
}

static void show_result(const perf_state *s, int which) {
    char last[48], best[48];
    describe(which, &s->last[which], last, sizeof last);
    describe(which, &s->best[which], best, sizeof best);
    hsfz_text_once((unsigned)(M_FIRST_RESULT + which), "%s      last %s      best %s",
                   names[which], last, best);
}

static void load(perf_state *s, const char *path) {
    FILE *f = path ? fopen(path, "r") : 0;
    char line[160];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int which;
        perf_result r;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "best %d %lf %lf %lf %lf", &which, &r.seconds, &r.trap_kph, &r.meters,
                   &r.decel_g) != 5 || which < 0 || which >= PERF_COUNT ||
            !(r.seconds > 0 && r.seconds < 100))
            continue;
        r.valid = 1;
        s->best[which] = r;
    }
    fclose(f);
}

static void save(const perf_state *s, const char *path) {
    char temporary[256];
    FILE *f;
    int i;
    if (!path || snprintf(temporary, sizeof temporary, "%s.new", path) >= (int)sizeof temporary) return;
    f = fopen(temporary, "w");
    if (!f) return;
    for (i = 0; i < PERF_COUNT; ++i)
        if (s->best[i].valid)
            fprintf(f, "best %d %.4f %.3f %.3f %.4f\n", i, s->best[i].seconds, s->best[i].trap_kph,
                    s->best[i].meters, s->best[i].decel_g);
    if (fclose(f) == 0) rename(temporary, path);
    else remove(temporary);
}

static void status(const perf_state *s, int connected, int fresh, uint64_t now) {
    int phase = perf_phase(s);
    double speed = mph(s->kph), elapsed = perf_elapsed(s, now);
    if (!connected) { hsfz_text_once(M_STATUS, "Connecting to the engine..."); return; }
    if (!fresh) { hsfz_text_once(M_STATUS, "Waiting for speed data"); return; }
    switch (phase) {
    case PERF_READY:
        hsfz_text_once(M_STATUS, "Ready: launch from a stop, or brake hard from above 60 mph");
        break;
    case PERF_LAUNCH:
        hsfz_text_once(M_STATUS, "Launch   %.1f s   %.0f mph", elapsed, speed);
        break;
    case PERF_ROLL:
        hsfz_text_once(M_STATUS, "Rolling from 60 mph   %.1f s   %.0f mph", elapsed, speed);
        break;
    case PERF_BRAKING:
        hsfz_text_once(M_STATUS, "Braking   %.1f s   %.0f mph", elapsed, speed);
        break;
    default:
        hsfz_text_once(M_STATUS, "%.0f mph   Stop fully to arm a launch", speed);
    }
}

int main(int argc, char **argv) {
    const char *host = 0, *state_path = 0;
    unsigned port = HSFZ_PORT, cycles = 0, count = 0;
    int reset = 0, i;
    pid_t parent = getppid();
    hsfz_session session;
    perf_state state;
    uint64_t sampled = 0, shown = 0;
    int last_look = -1;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc && !host) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = hsfz_number(argv[++i], 1, 65535);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = hsfz_number(argv[++i], 1, 1000000);
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state_path = argv[++i];
        else if (!strcmp(argv[i], "--reset")) reset = 1;
        else {
            fprintf(stderr, "Usage: %s --host IPv4 [--port 6801] [--state FILE] [--reset] [--cycles N]\n", argv[0]);
            return 2;
        }
    }
    if (!host) return 2;
    hsfz_signals();
    perf_init(&state);
    if (reset && state_path) remove(state_path);
    else load(&state, state_path);
    hsfz_session_init(&session, host, port);
    hsfz_text(M_NOTE, "Speed comes from the engine's OBD data: times are about +/- 0.1 s. "
                      "Closed course only.");
    for (i = 0; i < PERF_COUNT; ++i) show_result(&state, i);
    while (hsfz_running && getppid() == parent && (!cycles || count < cycles)) {
        uint64_t start = bd_mono_ms();
        int fd = hsfz_session_fd(&session);
        if (fd >= 0) {
            unsigned char data[4];
            int n = hsfz_obd(fd, 0x12, 0x0d, data, sizeof data, 250);
            if (n == 1) {
                unsigned done;
                sampled = bd_mono_ms();
                done = perf_sample(&state, sampled, data[0]);
                for (i = 0; i < PERF_COUNT; ++i)
                    if (done & (1u << i)) show_result(&state, i);
                if (done) save(&state, state_path);
            }
            hsfz_session_done(&session, n != -1);
        }
        {
            uint64_t now = bd_mono_ms();
            int fresh = sampled && now - sampled < 600;
            int look = (sampled != 0) | fresh << 1 | perf_phase(&state) << 2;
            if (!fresh && state.have) perf_sample(&state, now, -1);
            /* Redraw at once on a change of phase, else five times a second. */
            if (look != last_look || now - shown >= 200) {
                status(&state, sampled != 0, fresh, now);
                shown = now;
                last_look = look;
            }
        }
        ++count;
        {
            uint64_t elapsed = bd_mono_ms() - start;
            /* Poll as fast as the gateway answers, without spinning. */
            hsfz_pause(elapsed < 30 ? (unsigned)(30 - elapsed) : 5);
        }
    }
    hsfz_session_close(&session);
    return 0;
}
