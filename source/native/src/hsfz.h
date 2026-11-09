/* HSFZ (BMW diagnostics over TCP 6801) transport shared by every worker process.
 * Workers run outside HmiMain3D, so blocking here never stalls the iDrive UI. */
#ifndef HSFZ_H
#define HSFZ_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

enum { HSFZ_PORT = 6801, HSFZ_TESTER = 0xf4, HSFZ_FUNCTIONAL = 0xdf };

extern volatile sig_atomic_t hsfz_running;
void hsfz_stop(int sig);
/* SIGINT and SIGTERM end the worker loop; SIGPIPE is ignored. */
void hsfz_signals(void);

int hsfz_ready(int fd, short events, uint64_t deadline);
int hsfz_transfer(int fd, unsigned char *buf, size_t n, int sending, uint64_t deadline);
/* Returns a connected, non-blocking socket or -1. */
int hsfz_connect(const char *host, unsigned port);

/* One physical request. Returns the reply length (copied into reply, starting at
 * the positive response SID), -2 for a negative response (code in *negative), or
 * -1 for a transport failure or timeout. Response-pending (NRC 78) keeps waiting. */
int hsfz_exchange(int fd, unsigned ecu, const unsigned char *uds, unsigned uds_size,
                  const unsigned char *prefix, unsigned prefix_size,
                  unsigned char *reply, unsigned reply_size, unsigned timeout_ms,
                  unsigned *negative);

/* OBD service 01 PID read from the DME. Returns the data length after 41 PID,
 * -2 for a refusal, -1 for a transport failure. */
int hsfz_obd(int fd, unsigned ecu, unsigned pid, unsigned char *data, unsigned size,
             unsigned timeout_ms);
/* UDS 22 DID read. Returns the data length after 62 DID, -2 or -1 as above. */
int hsfz_did(int fd, unsigned ecu, unsigned did, unsigned char *data, unsigned size,
             unsigned timeout_ms);

/* A functional request to every control unit (target DF). Each positive or
 * negative reply for service uds[0] is passed to the callback with its source
 * address. Collection ends idle_ms after the latest reply (or after idle_ms with
 * no reply at all) and never later than total_ms. Returns the number of replies,
 * or -1 if the connection failed. */
typedef void (*hsfz_reply_fn)(void *context, unsigned source, const unsigned char *uds,
                              unsigned size);
int hsfz_functional(int fd, const unsigned char *uds, unsigned uds_size, unsigned idle_ms,
                    unsigned total_ms, hsfz_reply_fn callback, void *context);

/* A gateway connection shared fairly with the other apps' workers through the
 * loopback lock in diagnostic_lease.h. */
typedef struct {
    const char *host;
    unsigned port;
    int fd, lock;
    uint64_t lease_until, retry_at;
} hsfz_session;
void hsfz_session_init(hsfz_session *s, const char *host, unsigned port);
/* Returns the connected socket, or -1 while the gateway is busy or unreachable. */
int hsfz_session_fd(hsfz_session *s);
/* Report the outcome of the requests made on the socket: a transport failure
 * drops the connection; otherwise the lease is handed on when another app waits. */
void hsfz_session_done(hsfz_session *s, int transport_ok);
void hsfz_session_close(hsfz_session *s);

/* Worker output for text_app.h. A failed write (the plugin went away) ends the
 * worker loop. */
void hsfz_text(unsigned model, const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* As hsfz_text, but only writes when the model's text changed (models < 256). */
void hsfz_text_once(unsigned model, const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void hsfz_image(unsigned model, unsigned image);
/* A value for the plugin itself (text_app.h V line). */
void hsfz_value_line(unsigned key, long number);
/* As hsfz_image, but only writes when the model's image changed. */
void hsfz_image_once(unsigned model, unsigned image);

/* A value with several possible sources, tried in order (the one that answered
 * last goes first). obd=1: OBD service 01 PID `id`; obd=0: UDS 22 DID `id`.
 * value = raw * factor + offset, where raw is `bytes` big-endian bytes starting
 * `at` bytes into the data. Values outside [min, max] count as unavailable. */
typedef struct {
    unsigned ecu;
    int obd;
    unsigned id, at, bytes;
    int is_signed;
    double factor, offset, min, max;
} hsfz_candidate;
/* Returns 1 with *value set, 0 if no candidate answered plausibly, -1 if the
 * connection failed. */
int hsfz_value(int fd, const hsfz_candidate *list, unsigned count, unsigned *pick,
               double *value);
/* Sleep that wakes early when the worker is told to stop. */
void hsfz_pause(unsigned ms);

/* Parse a decimal command-line value or exit with status 2. */
unsigned hsfz_number(const char *s, unsigned lo, unsigned hi);
/* Parse a hexadecimal control unit address (00-FF) or exit with status 2. */
unsigned hsfz_address(const char *s);
#endif
