#include "drive_core.h"
#include <math.h>
#include <string.h>

/* ---- Warm-Up ---------------------------------------------------------- */
void wu_init(wu_state *s) { memset(s, 0, sizeof *s); }

void wu_update(wu_state *s, uint64_t now, double oil, double coolant, int rpm, wu_result *out) {
    double g, limit;
    unsigned i, last;
    memset(out, 0, sizeof *out);
    out->eta_s = -1;
    if (!isnan(oil)) g = oil;
    else if (!isnan(coolant)) { g = coolant - 10; out->from_coolant = 1; } /* coolant leads oil */
    else { out->stage = WU_WAITING; return; }
    out->governing_c = g;
    last = (s->next + WU_HISTORY - 1) % WU_HISTORY;
    if (!s->count || now - s->t[last] >= 5000) {
        s->t[s->next] = now;
        s->temp[s->next] = g;
        s->next = (s->next + 1) % WU_HISTORY;
        if (s->count < WU_HISTORY) ++s->count;
    }
    {
        double p = (g - WU_START_C) * 100.0 / (WU_WARM_C - WU_START_C);
        out->percent = p <= 0 ? 0u : p >= 100 ? 100u : (unsigned)p;
    }
    if (g >= WU_WARM_C) {
        out->stage = WU_WARM;
        return;
    }
    /* 3,000 rpm at 30 C or below, rising to 6,000 rpm at 90 C, in 250 rpm steps. */
    limit = 3000 + (g - 30) * 3000.0 / 60.0;
    if (limit < 3000) limit = 3000;
    if (limit > 6000) limit = 6000;
    out->limit_rpm = (unsigned)(limit / 250) * 250;
    out->stage = g < 50 ? WU_COLD : g < 75 ? WU_WARMING : WU_NEARLY;
    out->over_limit = rpm >= 0 && (unsigned)rpm > out->limit_rpm;
    /* Rate over the oldest sample between one and three minutes old. */
    for (i = 0; i < s->count; ++i) {
        unsigned k = (s->next + WU_HISTORY - s->count + i) % WU_HISTORY;
        uint64_t age = now - s->t[k];
        if (age > 180000) continue;
        if (age < 60000) break;
        {
            double per_ms = (g - s->temp[k]) / (double)age;
            if (per_ms * 60000.0 >= 0.2) out->eta_s = (int)((WU_WARM_C - g) / per_ms / 1000.0 + 0.5);
        }
        break;
    }
}

/* ---- Performance Timer ------------------------------------------------ */
#define MPH60 96.56064
#define MPH100 160.9344
#define MPH130 209.21472
#define QUARTER_M 402.336
#define GRAVITY 9.80665

void perf_init(perf_state *s) { memset(s, 0, sizeof *s); }

static void cancel_runs(perf_state *s) {
    s->launch_active = s->roll_active = s->brake_active = 0;
}

int perf_better(int which, const perf_result *r, const perf_result *best) {
    if (!r->valid) return 0;
    if (!best->valid) return 1;
    if (which == PERF_BRAKE) return r->meters < best->meters;
    return r->seconds < best->seconds;
}

static unsigned record(perf_state *s, int which, double seconds, double trap, double meters,
                       double decel) {
    perf_result r;
    r.valid = 1;
    r.seconds = seconds;
    r.trap_kph = trap;
    r.meters = meters;
    r.decel_g = decel;
    s->last[which] = r;
    if (perf_better(which, &r, &s->best[which])) s->best[which] = r;
    return 1u << which;
}

/* Time at which speed crossed `target` between two samples. */
static double crossing(double t0, double dt, double v0, double v1, double target) {
    return v1 == v0 ? t0 + dt : t0 + (target - v0) / (v1 - v0) * dt;
}

unsigned perf_sample(perf_state *s, uint64_t t, int kph) {
    double v0, v1 = kph, t0, dt, seg;
    unsigned done = 0;
    if (kph < 0) { s->have = 0; cancel_runs(s); return 0; }
    if (!s->have) {
        s->have = 1;
        s->t = t;
        s->kph = v1;
        s->launch_armed = kph == 0;
        return 0;
    }
    if (t <= s->t) return 0;
    dt = (double)(t - s->t);
    t0 = (double)s->t;
    v0 = s->kph;
    s->t = t;
    s->kph = v1;
    if (dt > 600) {
        cancel_runs(s);
        s->launch_armed = kph == 0;
        return 0;
    }
    seg = (v0 + v1) / 2 / 3.6 * dt / 1000;

    /* Launch from a standstill: 0-60 mph and the quarter mile. */
    if (s->launch_active) {
        double before = s->launch_dist;
        s->launch_dist += seg;
        if (v1 > s->launch_max) s->launch_max = v1;
        if (!s->launch_60 && v0 < MPH60 && v1 >= MPH60) {
            double secs = (crossing(t0, dt, v0, v1, MPH60) - s->launch_start) / 1000;
            s->launch_60 = 1;
            if (secs < 20) done |= record(s, PERF_0_60, secs, 0, 0, 0);
        }
        if (before < QUARTER_M && s->launch_dist >= QUARTER_M) {
            double f = (QUARTER_M - before) / seg;
            double secs = (t0 + f * dt - s->launch_start) / 1000;
            if (secs < 30) done |= record(s, PERF_QUARTER, secs, v0 + f * (v1 - v0), 0, 0);
            s->launch_active = 0;
        } else if (kph == 0 || v1 < s->launch_max - 8) {
            s->launch_active = 0; /* lifted or braked: the run is over */
        }
    } else if (s->launch_armed && v0 == 0 && v1 > 0) {
        double start = crossing(t0, dt, 0, v1, 0.5);
        s->launch_active = 1;
        s->launch_60 = 0;
        s->launch_start = start;
        s->launch_max = v1;
        s->launch_dist = (0.5 + v1) / 2 / 3.6 * ((double)t - start) / 1000;
        s->launch_armed = 0;
    }
    if (kph == 0) s->launch_armed = 1;

    /* Rolling 60-100 and 60-130 mph. */
    if (s->roll_active) {
        if (v1 > s->roll_max) s->roll_max = v1;
        if (!s->roll_100 && v0 < MPH100 && v1 >= MPH100) {
            double secs = (crossing(t0, dt, v0, v1, MPH100) - s->roll_start) / 1000;
            s->roll_100 = 1;
            if (secs < 30) done |= record(s, PERF_60_100, secs, 0, 0, 0);
        }
        if (v0 < MPH130 && v1 >= MPH130) {
            double secs = (crossing(t0, dt, v0, v1, MPH130) - s->roll_start) / 1000;
            if (secs < 60) done |= record(s, PERF_60_130, secs, 0, 0, 0);
            s->roll_active = 0;
        } else if (v1 < s->roll_max - 5 || v1 < MPH60 - 6) {
            s->roll_active = 0;
        }
    } else if (v0 < MPH60 && v1 >= MPH60) {
        s->roll_active = 1;
        s->roll_100 = 0;
        s->roll_start = crossing(t0, dt, v0, v1, MPH60);
        s->roll_max = v1;
    }

    /* Braking from 60 mph to a stop. */
    if (s->brake_active) {
        if (v1 < s->brake_min) s->brake_min = v1;
        if (v1 > s->brake_min + 3 || (double)t - s->brake_start > 12000) {
            s->brake_active = 0;
        } else if (kph == 0) {
            double stop = v0 > 0.5 ? crossing(t0, dt, v0, 0, 0.5) : t0;
            double meters = s->brake_dist + (v0 + 0.5) / 2 / 3.6 * (stop - t0) / 1000;
            double secs = (stop - s->brake_start) / 1000;
            double decel = secs > 0 ? MPH60 / 3.6 / secs / GRAVITY : 0;
            /* Ignore ordinary stops: only firm braking counts as a test. */
            if (decel >= 0.35) done |= record(s, PERF_BRAKE, secs, 0, meters, decel);
            s->brake_active = 0;
        } else {
            s->brake_dist += seg;
        }
    } else if (v0 > MPH60 && v1 <= MPH60 && v1 > 0) {
        double start = crossing(t0, dt, v0, v1, MPH60);
        s->brake_active = 1;
        s->brake_start = start;
        s->brake_min = v1;
        s->brake_dist = (MPH60 + v1) / 2 / 3.6 * ((double)t - start) / 1000;
    }
    return done;
}

int perf_phase(const perf_state *s) {
    if (!s->have) return PERF_IDLE;
    if (s->launch_active) return PERF_LAUNCH;
    if (s->brake_active) return PERF_BRAKING;
    if (s->roll_active) return PERF_ROLL;
    if (s->launch_armed && s->kph == 0) return PERF_READY;
    return PERF_IDLE;
}

double perf_elapsed(const perf_state *s, uint64_t now) {
    double start;
    if (s->launch_active) start = s->launch_start;
    else if (s->brake_active) start = s->brake_start;
    else if (s->roll_active) start = s->roll_start;
    else return -1;
    return ((double)now - start) / 1000;
}

/* ---- Gear estimate ---------------------------------------------------- */
const double gear_ratios[GEAR_COUNT] = {5.000, 3.200, 2.143, 1.720, 1.314, 1.000, 0.822, 0.640};
#define FACTOR_MIN 12.0
#define FACTOR_MAX 45.0

static double bin_width(void) { return log(FACTOR_MAX / FACTOR_MIN) / GEAR_BINS; }

void gear_init(gear_state *s, double saved) {
    memset(s, 0, sizeof *s);
    if (saved >= FACTOR_MIN && saved <= FACTOR_MAX) s->factor = saved;
}

static unsigned score(const gear_state *s, int b) {
    unsigned total = s->votes[b];
    if (b > 0) total += s->votes[b-1];
    if (b + 1 < GEAR_BINS) total += s->votes[b+1];
    return total;
}

static unsigned gear_bits(const gear_state *s, int b) {
    unsigned bits = s->gears[b];
    if (b > 0) bits |= s->gears[b-1];
    if (b + 1 < GEAR_BINS) bits |= s->gears[b+1];
    return bits;
}

static int bit_count(unsigned bits) {
    int n = 0;
    for (; bits; bits &= bits - 1) ++n;
    return n;
}

/* Every gear in a correct calibration votes for the same factor; a wrong gear
 * alignment only agrees by coincidence. Accept a clear peak backed by at least
 * three different gears. */
static void evaluate(gear_state *s, int *learned) {
    int b, best = -1;
    unsigned top = 0, rival = 0;
    double factor;
    for (b = 0; b < GEAR_BINS; ++b) {
        unsigned v = score(s, b);
        if (v > top) { top = v; best = b; }
    }
    if (best < 0 || top < 25 || bit_count(gear_bits(s, best)) < 3) return;
    for (b = 0; b < GEAR_BINS; ++b)
        if ((b < best - 3 || b > best + 3) && score(s, b) > rival) rival = score(s, b);
    if (rival * 10 > top * 6) return;
    factor = FACTOR_MIN * exp((best + 0.5) * bin_width());
    if (!s->factor || fabs(log(factor / s->factor)) > 0.03) {
        s->factor = factor;
        *learned = 1;
    }
}

int gear_update(gear_state *s, int rpm, int kph, int *learned) {
    double ratio, best_error = 1e9, tolerance;
    int g, gear = 0;
    *learned = 0;
    if (rpm < 500 || kph < 5) { s->stable = 0; s->last_ratio = 0; return 0; }
    ratio = (double)rpm / kph;
    if (s->last_ratio > 0 && fabs(ratio / s->last_ratio - 1) < 0.015) ++s->stable;
    else s->stable = 0;
    s->last_ratio = ratio;
    if (s->stable >= 3 && kph >= 40 && rpm >= 900) {
        int voted = 0;
        for (g = 0; g < GEAR_COUNT; ++g) {
            double factor = ratio / gear_ratios[g];
            int b;
            if (factor < FACTOR_MIN || factor >= FACTOR_MAX) continue;
            b = (int)(log(factor / FACTOR_MIN) / bin_width());
            if (b < 0 || b >= GEAR_BINS) continue;
            if (++s->votes[b] >= 60000) {
                int k;
                for (k = 0; k < GEAR_BINS; ++k) s->votes[k] /= 2;
            }
            s->gears[b] |= (unsigned char)(1u << g);
            voted = 1;
        }
        if (voted && ++s->samples % 10 == 0) evaluate(s, learned);
    }
    if (!s->factor) return 0;
    /* Speed is reported in whole km/h, so allow more error at low speed. */
    tolerance = 0.06 + 1.0 / kph;
    for (g = 0; g < GEAR_COUNT; ++g) {
        double error = fabs(log(ratio / (s->factor * gear_ratios[g])));
        if (error < best_error) { best_error = error; gear = g + 1; }
    }
    return best_error <= log(1 + tolerance) ? gear : 0;
}
