/* Pure logic for Warm-Up, Performance Timer and the ShiftDeck gear indicator.
 * No I/O, so every rule here is covered by tests/test_core.c on the host. */
#ifndef DRIVE_CORE_H
#define DRIVE_CORE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- Warm-Up ---------------------------------------------------------- */
enum { WU_HISTORY = 64 };
enum { WU_WAITING, WU_COLD, WU_WARMING, WU_NEARLY, WU_WARM };
typedef struct {
    uint64_t t[WU_HISTORY];
    double temp[WU_HISTORY];
    unsigned count, next;
} wu_state;
typedef struct {
    int stage;          /* WU_* */
    int from_coolant;   /* oil temperature unavailable: estimated from coolant */
    double governing_c; /* temperature the advice is based on */
    unsigned limit_rpm; /* 0 when warm (no limit) */
    unsigned percent;   /* 0..100 of the way to working temperature */
    int eta_s;          /* seconds to working temperature, -1 if unknown */
    int over_limit;     /* rpm above the advised limit */
} wu_result;
enum { WU_WARM_C = 90, WU_START_C = 20 };
void wu_init(wu_state *s);
/* oil_c / coolant_c are NaN when unavailable; rpm < 0 when unavailable. */
void wu_update(wu_state *s, uint64_t now_ms, double oil_c, double coolant_c, int rpm,
               wu_result *out);

/* ---- Performance Timer ------------------------------------------------ */
enum { PERF_0_60, PERF_QUARTER, PERF_60_100, PERF_60_130, PERF_BRAKE, PERF_COUNT };
enum { PERF_IDLE, PERF_READY, PERF_LAUNCH, PERF_ROLL, PERF_BRAKING };
typedef struct {
    int valid;
    double seconds;
    double trap_kph;  /* quarter mile */
    double meters;    /* braking distance */
    double decel_g;   /* braking */
} perf_result;
typedef struct {
    int have;
    uint64_t t;
    double kph;
    int launch_armed, launch_active, launch_60;
    double launch_start, launch_dist, launch_max;
    int roll_active, roll_100;
    double roll_start, roll_max;
    int brake_active;
    double brake_start, brake_dist, brake_min;
    perf_result last[PERF_COUNT], best[PERF_COUNT];
} perf_state;
void perf_init(perf_state *s);
/* Feed one speed sample (whole km/h, as OBD PID 0D reports it). Returns a bit
 * mask of PERF_* results completed by this sample. A gap over 600 ms between
 * samples cancels any run in progress. */
unsigned perf_sample(perf_state *s, uint64_t t_ms, int kph);
int perf_phase(const perf_state *s);
/* Elapsed seconds of the run in progress, or -1. */
double perf_elapsed(const perf_state *s, uint64_t now_ms);
/* Smaller is better for every result; returns 1 if r became the new best. */
int perf_better(int which, const perf_result *r, const perf_result *best);

/* ---- Gear estimate (ZF 8HP45 / 8HP50) ---------------------------------- */
enum { GEAR_BINS = 128, GEAR_COUNT = 8 };
typedef struct {
    double factor;        /* engine rpm per km/h in a 1.000 ratio; 0 = not learned */
    unsigned votes[GEAR_BINS];
    unsigned char gears[GEAR_BINS]; /* bit g-1: gear g voted for this bin */
    unsigned samples;
    double last_ratio;
    unsigned stable;
} gear_state;
void gear_init(gear_state *s, double saved_factor);
/* Returns the gear 1..8, or 0 while unknown (learning, shifting, clutch or
 * converter slip, too slow). *learned is set to 1 when this sample completed or
 * changed the calibration, so the caller can save gear_state.factor. */
int gear_update(gear_state *s, int rpm, int kph, int *learned);
extern const double gear_ratios[GEAR_COUNT];

#ifdef __cplusplus
}
#endif
#endif
