/* Warm-Up worker: oil and coolant temperature with a rising rev limit.
 * Read only. Output goes to the WarmUp plugin through text_app.h lines. */
#define _POSIX_C_SOURCE 200809L
#include "app_assets.h"
#include "app_worker.h"
#include "drive_core.h"
#include "gauge_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { M_OIL = 50, M_COOLANT, M_STAGE, M_LIMIT, M_ETA, M_NOTE, M_BAR = 60 };

static const app_candidate oil_sources[] = {
    {0x12, 1, 0x5c, 0, 1, 0, 1, -40, -40, 170},     /* OBD engine oil temperature */
    {0x12, 0, 0xf45c, 0, 1, 0, 1, -40, -40, 170},   /* the same PID as a UDS DID */
};
static const app_candidate rpm_sources[] = {
    {0x12, 1, 0x0c, 0, 2, 0, 0.25, 0, 0, 10000},
};

static double fahrenheit(double c) { return c * 9 / 5 + 32; }

static void temperature(unsigned model, const char *name, double c) {
    if (isnan(c)) app_text_once(model, "%s   --", name);
    else app_text_once(model, "%s   %.0f\260F   (%.0f\260C)", name, fahrenheit(c), c);
}

static void show(const wu_result *r, double oil, double coolant, int rpm, int connected) {
    static const char *const stages[] = {
        "Waiting for temperature data", "Cold: take it easy", "Warming up",
        "Nearly warm", "Warm: full rev range available"};
    unsigned step = r->percent * WU_BAR_STEPS / 100;
    temperature(M_OIL, "Engine oil", oil);
    temperature(M_COOLANT, "Coolant", coolant);
    if (!connected) app_text_once(M_STAGE, "Connecting to the engine...");
    else if (r->over_limit) app_text_once(M_STAGE, "Above the warm-up limit: ease off");
    else app_text_once(M_STAGE, "%s", stages[r->stage]);
    if (r->stage == WU_WAITING) app_text_once(M_LIMIT, "Suggested limit   --");
    else if (!r->limit_rpm) app_text_once(M_LIMIT, "Suggested limit   none");
    else if (rpm >= 0) app_text_once(M_LIMIT, "Suggested limit   %u rpm   (now %d rpm)",
                                     r->limit_rpm, (rpm + 25) / 50 * 50);
    else app_text_once(M_LIMIT, "Suggested limit   %u rpm", r->limit_rpm);
    if (r->stage == WU_WARM) app_text_once(M_ETA, "Oil at working temperature");
    else if (r->eta_s >= 0) app_text_once(M_ETA, "About %d min to working temperature",
                                          (r->eta_s + 59) / 60);
    else app_text_once(M_ETA, "Time to working temperature: measuring...");
    if (r->from_coolant) app_text_once(M_NOTE, "Oil temperature unavailable: estimated from coolant");
    else app_text_once(M_NOTE, "Working temperature: oil at %d\260F", WU_WARM_C * 9 / 5 + 32);
    app_image_once(M_BAR, (r->stage == WU_WARM ? WU_BAR_WARM : WU_BAR_BASE) + step);
}

int main(int argc, char **argv) {
    struct diag_link_cfg cfg = {0, 6801, DIAG_AUTO, DIAG_LOOP, "warmup", 1};
    struct diag_link link;
    unsigned cycles = 0, count = 0, oil_pick = 0, rpm_pick = 0;
    pid_t parent = getppid();
    wu_state state;
    double oil = NAN, coolant = NAN;
    int rpm = -1, i, lost;
    uint64_t oil_at = 0, coolant_at = 0, rpm_at = 0;
    for (i = 1; i < argc; ++i) {
        if (app_link_option(argc, argv, &i, &cfg)) continue;
        if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = app_number(argv[++i], 1, 100000);
        else {
            fprintf(stderr, "Usage: %s --host IPv4 [--transport auto|hsfz|most] [--port 6801] [--cycles N]\n",
                    argv[0]);
            return 2;
        }
    }
    if ((!cfg.host && cfg.transport != DIAG_MOST) || parent <= 1) return 2;
    diag_link_signals();
    diag_link_init(&link, &cfg);
    wu_init(&state);
    while (diag_running && getppid() == parent && (!cycles || count < cycles) && !diag_link_lost(&link)) {
        uint64_t start = bd_mono_ms();
        wu_result result;
        diag_link_tick(&link);
        if (diag_link_up(&link)) {
            unsigned char data[8];
            double value;
            int ok = 1, rc = app_value(&link, oil_sources, 2, &oil_pick, &value);
            if (rc < 0) ok = 0;
            else if (rc) { oil = value; oil_at = start; }
            if (ok) {
                rc = app_did(&link, 0x12, 0x4300, data, sizeof data, 300);
                if (rc == -1) ok = 0;
                else if (rc >= 0 && bd_decode_did(0x4300, data, (size_t)rc, &value)) {
                    coolant = value;
                    coolant_at = start;
                }
            }
            if (ok) {
                rc = app_value(&link, rpm_sources, 1, &rpm_pick, &value);
                if (rc < 0) ok = 0;
                else if (rc) { rpm = (int)value; rpm_at = start; }
            }
            app_link_done(&link, ok);
        }
        /* Forget readings that stopped updating. */
        if (start - oil_at > 5000) oil = NAN;
        if (start - coolant_at > 5000) coolant = NAN;
        if (start - rpm_at > 3000) rpm = -1;
        wu_update(&state, start, oil, coolant, rpm, &result);
        show(&result, oil, coolant, rpm, oil_at || coolant_at);
        ++count;
        if (!cycles || count < cycles) {
            uint64_t elapsed = bd_mono_ms() - start;
            app_pause(elapsed < 1000 ? (unsigned)(1000 - elapsed) : 50);
        }
    }
    lost = diag_link_lost(&link);
    diag_link_close(&link);
    return lost ? 1 : 0;
}
