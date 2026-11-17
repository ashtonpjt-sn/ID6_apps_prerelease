/* The diagnostic link every worker uses: HSFZ to the gateway, or the MOST UDS tunnel when HSFZ cannot be
 * reached. One link per process, used from one thread. */
#ifndef DIAG_LINK_H
#define DIAG_LINK_H

#include <signal.h>
#include <stdint.h>

enum { DIAG_AUTO, DIAG_HSFZ, DIAG_MOST };   /* transport */
enum { DIAG_LOOP, DIAG_ONESHOT };           /* kind */
struct diag_link_cfg { const char *host; unsigned port; int transport; int kind;
                       const char *dsi_name; int session; };
struct diag_link {
    struct diag_link_cfg cfg;
    int fd, lock;                     /* HSFZ connection and lease lock, -1 when closed */
    int on_most, most_usable, lost;
    int hsfz_seen;                    /* HSFZ has connected: never switch to MOST */
    unsigned hsfz_fails;              /* failed HSFZ connects in a row */
    uint64_t retry_at, lease_until, most_retry_at;
    uint64_t most_yield_until;        /* a loop worker on MOST waits until then before its next request */
};

extern volatile sig_atomic_t diag_running;          /* cleared by SIGINT and SIGTERM */
void diag_link_init(struct diag_link *, const struct diag_link_cfg *);
/* SIGINT/SIGTERM clear diag_running, arming alarm(15) once MOST has been used; SIGPIPE is ignored and
   SIGALRM keeps its default action. */
void diag_link_signals(void);
/* 1 when usable (HSFZ connected or MOST up), 0 not yet. Connects, retries and switches to MOST. */
int  diag_link_up(struct diag_link *);
/* The UDS reply length; -1 link lost; -2 negative reply, *negative holding its code; -3 this request failed
   but the link is kept (MOST only). */
int  diag_link_request(struct diag_link *, unsigned ecu, const unsigned char *uds, unsigned n,
                       const unsigned char *prefix, unsigned pn, unsigned char *reply, unsigned cap,
                       unsigned timeout_ms, unsigned *negative);
void diag_link_drop(struct diag_link *);       /* HSFZ: release fd+lock, retry later; MOST: no-op */
int  diag_link_on_most(const struct diag_link *);
int  diag_link_lost(const struct diag_link *); /* MOST link gone: a loop worker should exit 1 */
/* Session mode on HSFZ: 1 when the lease should go to a waiting worker; the link has then released it
   and backs off. 0 on MOST. */
int  diag_link_should_yield(struct diag_link *, uint64_t now);
void diag_link_tick(struct diag_link *);       /* arms the 60 s loop alarm while on MOST */
void diag_link_close(struct diag_link *);      /* bounded shutdown */
/* Plain HSFZ, for workers that never use MOST: a connected fd or -1, and one request on it with
   diag_link_request's return values, without -3. Both stop early once diag_running is cleared. */
int  diag_hsfz_connect(const char *host, unsigned port);
int  diag_hsfz_exchange(int fd, unsigned ecu, const unsigned char *uds, unsigned n,
                        const unsigned char *prefix, unsigned pn, unsigned char *reply, unsigned cap,
                        unsigned timeout_ms, unsigned *negative);
/* A functional request to every control unit (target DF), HSFZ only. Each positive or negative reply to
   service uds[0] goes to the callback with its source address; a unit still working (NRC 78) extends the
   wait by 2 s. Collection ends idle_ms after the latest reply, or after idle_ms with none, and never later
   than total_ms. The number of replies, or -1 when the connection failed. */
typedef void (*diag_reply_fn)(void *context, unsigned source, const unsigned char *uds, unsigned size);
int  diag_hsfz_functional(int fd, const unsigned char *uds, unsigned n, unsigned idle_ms,
                          unsigned total_ms, diag_reply_fn callback, void *context);
/* One-shot: hold the lease for the run, retrying up to 2000 ms. 0 when not held, errno from the last try. */
int  diag_link_reserve_oneshot(struct diag_link *);

#endif
