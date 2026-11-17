/* Shared by the text-app workers (Warm-Up, Performance Timer, Fault Scanner, BoostDeck's More vitals).
 * Reads go through diag_link, so a loop worker falls back to the MOST tunnel like BoostDeck does.
 * Output goes to the plugin as text_app.h lines on stdout. */
#ifndef APP_WORKER_H
#define APP_WORKER_H

#include "diag_link.h"

/* Worker output, one update per line. A failed write (the plugin went away) clears diag_running. */
void app_text(unsigned model, const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* As app_text, but only writes when the model's text changed (models below 256). */
void app_text_once(unsigned model, const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void app_image(unsigned model, unsigned image);
void app_image_once(unsigned model, unsigned image);
/* A value for the plugin itself: text_app.h passes it to workerValue(). */
void app_value_line(unsigned key, long number);

/* A sleep that ends early once the worker is told to stop. */
void app_pause(unsigned ms);
/* A decimal option value, or exit with status 2. */
unsigned app_number(const char *s, unsigned lo, unsigned hi);
/* A hexadecimal control unit address 00-FF, or exit with status 2. */
unsigned app_address(const char *s);
/* Takes --host, --port and --transport auto|hsfz|most at argv[*i]. 1 when used (and *i moved past the
   value), 0 when argv[*i] is not one of them. */
int app_link_option(int argc, char **argv, int *i, struct diag_link_cfg *cfg);

/* The data after "41 PID" or "62 DID": its length, -2 when refused or unavailable for now, -1 when the
   link is lost and should be dropped. */
int app_obd(struct diag_link *link, unsigned ecu, unsigned pid, unsigned char *data, unsigned size,
            unsigned timeout_ms);
int app_did(struct diag_link *link, unsigned ecu, unsigned did, unsigned char *data, unsigned size,
            unsigned timeout_ms);

/* A value with several possible sources, tried in order starting with the one that answered last.
   obd 1: OBD service 01 PID id; obd 0: UDS 22 DID id. value = raw * factor + offset, where raw is
   `bytes` big-endian bytes starting `at` bytes into the data. Values outside [min, max] count as
   unavailable. */
typedef struct {
    unsigned ecu;
    int obd;
    unsigned id, at, bytes;
    int is_signed;
    double factor, offset, min, max;
} app_candidate;
/* 1 with *value set, 0 when no candidate answered plausibly, -1 when the link is lost. */
int app_value(struct diag_link *link, const app_candidate *list, unsigned count, unsigned *pick,
              double *value);

/* After a loop worker's reads: a lost link is dropped and retried later; otherwise the gateway lease goes
   to a waiting worker. */
void app_link_done(struct diag_link *link, int ok);

#endif
