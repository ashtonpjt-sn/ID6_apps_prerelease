/* Host tests for drive_core.c: Warm-Up advice, run timing and gear learning. */
#include "drive_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double actual, double expected, double tolerance) {
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr, "expected %.4f +/- %.4f, got %.4f\n", expected, tolerance, actual);
        assert(0);
    }
}

static void test_warmup(void) {
    wu_state s;
    wu_result r;
    uint64_t t = 1000;
    int i;
    wu_init(&s);
    wu_update(&s, t, NAN, NAN, 900, &r);
    assert(r.stage == WU_WAITING);
    wu_update(&s, t, 20, 30, 900, &r);
    assert(r.stage == WU_COLD && r.limit_rpm == 3000 && r.percent == 0 && !r.from_coolant);
    wu_update(&s, t, 20, 30, 3200, &r);
    assert(r.over_limit);
    wu_update(&s, t, 60, 80, 3200, &r);
    assert(r.stage == WU_WARMING && r.limit_rpm == 4500 && !r.over_limit);
    wu_update(&s, t, 80, 90, 3200, &r);
    assert(r.stage == WU_NEARLY && r.limit_rpm == 5500);
    wu_update(&s, t, 95, 90, 6800, &r);
    assert(r.stage == WU_WARM && r.limit_rpm == 0 && r.percent == 100 && !r.over_limit);
    /* Coolant only: advice from coolant minus 10 C. */
    wu_update(&s, t, NAN, 70, 1000, &r);
    assert(r.from_coolant && fabs(r.governing_c - 60) < 1e-9);

    /* 2 C per minute from 30 C: after 2 minutes at 34 C, 28 minutes to 90 C. */
    wu_init(&s);
    for (i = 0; i <= 120; ++i) {
        t = 10000 + (uint64_t)i * 1000;
        wu_update(&s, t, 30 + i / 30.0, NAN, 1000, &r);
    }
    assert(r.eta_s > 0);
    near(r.eta_s, 28 * 60, 30);
    /* Steady temperature: no estimate. */
    wu_init(&s);
    for (i = 0; i <= 120; ++i) wu_update(&s, 10000 + (uint64_t)i * 1000, 40, NAN, 800, &r);
    assert(r.eta_s == -1);
}

/* Drive a speed profile through the timer with OBD-style whole km/h samples. */
typedef double (*profile_fn)(double seconds);
static unsigned drive(perf_state *s, uint64_t *t, double seconds, unsigned step_ms, profile_fn speed) {
    unsigned done = 0;
    double x;
    for (x = 0; x <= seconds + 1e-9; x += step_ms / 1000.0) {
        double v = speed(x);
        done |= perf_sample(s, *t + (uint64_t)(x * 1000 + .5), (int)floor(v + 0.5));
    }
    *t += (uint64_t)(seconds * 1000 + .5) + step_ms;
    return done;
}

static double stopped(double x) { (void)x; return 0; }
/* Constant 0.55 g from rest: 0-60 mph in 4.97 s, quarter mile in about 12.2 s. */
static double launch(double x) { return 0.55 * 9.80665 * x * 3.6; }
/* 1.0 g braking from 70 mph. */
static double braking(double x) {
    double v = 70 * 1.609344 - 9.80665 * x * 3.6;
    return v > 0 ? v : 0;
}
/* A gentle stop from 70 mph at 0.15 g. */
static double gentle(double x) {
    double v = 70 * 1.609344 - 0.15 * 9.80665 * x * 3.6;
    return v > 0 ? v : 0;
}
/* Rolling acceleration from 55 mph at 0.35 g. */
static double roll(double x) { return 55 * 1.609344 + 0.35 * 9.80665 * x * 3.6; }

static void test_perf_launch_and_quarter(void) {
    perf_state s;
    uint64_t t = 5000;
    unsigned done;
    double expected_60 = 26.8224 / (0.55 * 9.80665);
    double expected_quarter = sqrt(2 * 402.336 / (0.55 * 9.80665));
    perf_init(&s);
    drive(&s, &t, 2, 50, stopped);
    assert(perf_phase(&s) == PERF_READY);
    done = drive(&s, &t, 13, 50, launch);
    assert(done & (1u << PERF_0_60));
    assert(done & (1u << PERF_QUARTER));
    near(s.last[PERF_0_60].seconds, expected_60, 0.08);
    near(s.last[PERF_QUARTER].seconds, expected_quarter, 0.08);
    near(s.last[PERF_QUARTER].trap_kph, 0.55 * 9.80665 * expected_quarter * 3.6, 2);
    assert(s.best[PERF_0_60].valid);
}

static void test_perf_braking(void) {
    perf_state s;
    uint64_t t = 1000;
    unsigned done;
    double v = 26.8224;
    perf_init(&s);
    done = drive(&s, &t, 4, 40, braking);
    assert(done & (1u << PERF_BRAKE));
    near(s.last[PERF_BRAKE].seconds, v / 9.80665, 0.08);
    near(s.last[PERF_BRAKE].meters, v * v / (2 * 9.80665), 1.0);
    near(s.last[PERF_BRAKE].decel_g, 1.0, 0.04);
    /* An ordinary stop is not a brake test and keeps the earlier result. */
    perf_init(&s);
    done = drive(&s, &t, 25, 40, gentle);
    assert(!(done & (1u << PERF_BRAKE)) && !s.last[PERF_BRAKE].valid);
}

static void test_perf_roll_and_gaps(void) {
    perf_state s;
    uint64_t t = 1000;
    unsigned done;
    double a = 0.35 * 9.80665 * 3.6; /* km/h per second */
    perf_init(&s);
    done = drive(&s, &t, 15, 50, roll);
    assert(done & (1u << PERF_60_100));
    assert(done & (1u << PERF_60_130));
    near(s.last[PERF_60_100].seconds, 40 * 1.609344 / a, 0.08);
    near(s.last[PERF_60_130].seconds, 70 * 1.609344 / a, 0.08);
    /* A long gap in the data cancels the run. */
    perf_init(&s);
    assert(!perf_sample(&s, 1000, 0));
    assert(!perf_sample(&s, 1050, 10));
    assert(perf_phase(&s) == PERF_LAUNCH);
    assert(!perf_sample(&s, 2000, 30));
    assert(perf_phase(&s) == PERF_IDLE);
    /* Unavailable speed cancels too. */
    assert(!perf_sample(&s, 2050, -1));
    assert(perf_phase(&s) == PERF_IDLE);
    /* Best keeps the faster run. */
    {
        perf_result slow = {1, 6.0, 0, 0, 0}, fast = {1, 5.0, 0, 0, 0};
        assert(perf_better(PERF_0_60, &fast, &slow));
        assert(!perf_better(PERF_0_60, &slow, &fast));
    }
}

static void test_gear_learning(void) {
    const double factor = 22.5;
    const int gears[] = {2, 3, 4, 5, 6, 7, 3, 5};
    gear_state s;
    int learned = 0, any = 0, i, g, k;
    gear_init(&s, 0);
    for (k = 0; k < 8; ++k) {
        g = gears[k];
        for (i = 0; i < 40; ++i) {
            int kph = 45 + 3 * k + (i % 5);
            int rpm = (int)(kph * factor * gear_ratios[g-1] + 0.5);
            (void)gear_update(&s, rpm, kph, &learned);
            any |= learned;
        }
    }
    assert(any && s.factor > 0);
    near(s.factor, factor, factor * 0.02);
    for (g = 1; g <= 8; ++g) {
        int kph = g == 1 ? 15 : 60, rpm = (int)(kph * factor * gear_ratios[g-1]);
        (void)gear_update(&s, rpm, kph, &learned);
        assert(gear_update(&s, rpm, kph, &learned) == g);
    }
    /* Slip between gears reads as unknown. */
    assert(gear_update(&s, (int)(60 * factor * 1.15), 60, &learned) == 0);
    /* Two gears alone are not enough to learn. */
    gear_init(&s, 0);
    for (k = 0; k < 200; ++k) {
        g = k < 100 ? 5 : 6;
        (void)gear_update(&s, (int)(80 * factor * gear_ratios[g-1]), 80, &learned);
        assert(!learned);
    }
    assert(s.factor == 0);
    /* A saved calibration is used straight away; nonsense is ignored. */
    gear_init(&s, factor);
    assert(gear_update(&s, (int)(50 * factor * gear_ratios[3]), 50, &learned) == 4);
    gear_init(&s, 400);
    assert(s.factor == 0);
}

int main(void) {
    test_warmup();
    test_perf_launch_and_quarter();
    test_perf_braking();
    test_perf_roll_and_gaps();
    test_gear_learning();
    puts("Drive core: warm-up advice, run timing, braking, rolls and gear learning passed.");
    return 0;
}
