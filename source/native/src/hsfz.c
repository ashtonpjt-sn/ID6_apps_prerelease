#define _POSIX_C_SOURCE 200809L
#include "hsfz.h"
#include "gauge_core.h"
#include "diagnostic_lease.h"
#include <math.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

volatile sig_atomic_t hsfz_running = 1;
void hsfz_stop(int sig) { (void)sig; hsfz_running = 0; }
void hsfz_signals(void) {
    signal(SIGINT, hsfz_stop);
    signal(SIGTERM, hsfz_stop);
    signal(SIGPIPE, SIG_IGN);
}

int hsfz_ready(int fd, short events, uint64_t deadline) {
    struct pollfd p;
    int rc;
    p.fd = fd; p.events = events; p.revents = 0;
    while (hsfz_running) {
        uint64_t now = bd_mono_ms();
        if (now >= deadline) return 0;
        rc = poll(&p, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (p.revents & events);
    }
    return 0;
}
int hsfz_transfer(int fd, unsigned char *buf, size_t n, int sending, uint64_t deadline) {
    while (n && hsfz_running) {
        ssize_t k;
        if (!hsfz_ready(fd, sending ? POLLOUT : POLLIN, deadline)) return 0;
        k = sending ? send(fd, buf, n, 0) : recv(fd, buf, n, 0);
        if (k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (k <= 0) return 0;
        buf += k; n -= (size_t)k;
    }
    return n == 0;
}
int hsfz_connect(const char *host, unsigned port) {
    struct sockaddr_in a;
    int fd, error = 0, no_delay = 1;
    socklen_t len = sizeof error;
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof no_delay);
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd); return -1;
    }
    if (!hsfz_ready(fd, POLLOUT, bd_mono_ms() + 1000) ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error) {
        close(fd); return -1;
    }
    return fd;
}

/* The fixed callers below supply the UDS service, target, and response prefix. */
int hsfz_exchange(int fd, unsigned ecu, const unsigned char *uds, unsigned uds_size,
                    const unsigned char *prefix, unsigned prefix_size,
                    unsigned char *reply, unsigned reply_size, unsigned timeout_ms,
                    unsigned *negative) {
    unsigned char request[12] = {0,0,0,0,0,1,0xf4,0};
    unsigned char header[6], body[4096];
    uint64_t deadline = bd_mono_ms() + timeout_ms;
    if (uds_size > sizeof request - 8) return -1;
    request[3] = (unsigned char)(uds_size+2);
    request[7] = (unsigned char)ecu;
    memcpy(request+8,uds,uds_size);
    *negative=0;
    if (!hsfz_transfer(fd,request,8+uds_size,1,deadline)) return -1;
    while (hsfz_running) {
        uint32_t len;
        unsigned ctrl;
        if (!hsfz_transfer(fd,header,6,0,deadline)) return -1;
        len=((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
        ctrl=((unsigned)header[4]<<8)|header[5];
        if (len>sizeof body || !hsfz_transfer(fd,body,len,0,deadline)) return -1;
        if (ctrl==0x12) {
            unsigned char alive[8]={0,0,0,2,0,0x13,0,0xf4};
            if (!hsfz_transfer(fd,alive,8,1,deadline)) return -1;
            continue;
        }
        if (ctrl!=1 || len<3 || body[0]!=ecu || body[1]!=0xf4) continue;
        if (len==5 && body[2]==0x7f && body[3]==uds[0]) {
            if (body[4]==0x78) continue;
            *negative=body[4];
            return -2;
        }
        if (len-2<prefix_size || memcmp(body+2,prefix,prefix_size)) continue;
        if (len-2>reply_size) return -1;
        memcpy(reply,body+2,len-2);
        return (int)(len-2);
    }
    return -1;
}

unsigned hsfz_number(const char *s, unsigned lo, unsigned hi) {
    char *end; unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n < lo || n > hi) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}

unsigned hsfz_address(const char *s) {
    char *end; unsigned long n = strtoul(s, &end, 16);
    if (!*s || *end || n > 0xff) { fprintf(stderr, "Invalid control unit address: %s\n", s); exit(2); }
    return (unsigned)n;
}

int hsfz_obd(int fd, unsigned ecu, unsigned pid, unsigned char *data, unsigned size,
             unsigned timeout_ms) {
    unsigned char request[2], prefix[2], reply[64];
    unsigned negative;
    int n;
    request[0]=0x01; request[1]=(unsigned char)pid;
    prefix[0]=0x41; prefix[1]=(unsigned char)pid;
    n=hsfz_exchange(fd,ecu,request,2,prefix,2,reply,sizeof reply,timeout_ms,&negative);
    if (n<0) return n;
    n-=2;
    if ((unsigned)n>size) n=(int)size;
    memcpy(data,reply+2,(size_t)n);
    return n;
}

int hsfz_did(int fd, unsigned ecu, unsigned did, unsigned char *data, unsigned size,
             unsigned timeout_ms) {
    unsigned char request[3], prefix[3], reply[256];
    unsigned negative;
    int n;
    request[0]=0x22; request[1]=(unsigned char)(did>>8); request[2]=(unsigned char)did;
    prefix[0]=0x62; prefix[1]=request[1]; prefix[2]=request[2];
    n=hsfz_exchange(fd,ecu,request,3,prefix,3,reply,sizeof reply,timeout_ms,&negative);
    if (n<0) return n;
    n-=3;
    if ((unsigned)n>size) n=(int)size;
    memcpy(data,reply+3,(size_t)n);
    return n;
}

int hsfz_functional(int fd, const unsigned char *uds, unsigned uds_size, unsigned idle_ms,
                    unsigned total_ms, hsfz_reply_fn callback, void *context) {
    unsigned char request[16] = {0,0,0,0,0,1,HSFZ_TESTER,HSFZ_FUNCTIONAL};
    unsigned char header[6], body[4096];
    uint64_t start = bd_mono_ms(), end = start + total_ms, idle = start + idle_ms;
    int replies = 0;
    if (uds_size > sizeof request - 8) return -1;
    request[3] = (unsigned char)(uds_size+2);
    memcpy(request+8,uds,uds_size);
    if (!hsfz_transfer(fd,request,8+uds_size,1,start+1000)) return -1;
    while (hsfz_running) {
        uint64_t deadline = idle < end ? idle : end, now = bd_mono_ms();
        uint32_t len;
        unsigned ctrl;
        if (now >= deadline) break;
        if (!hsfz_ready(fd,POLLIN,deadline)) {
            if (bd_mono_ms() >= deadline) break;
            return -1;
        }
        /* A frame that has started must arrive whole within a second. */
        if (!hsfz_transfer(fd,header,6,0,bd_mono_ms()+1000)) return -1;
        len=((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|header[3];
        ctrl=((unsigned)header[4]<<8)|header[5];
        if (len>sizeof body || !hsfz_transfer(fd,body,len,0,bd_mono_ms()+1000)) return -1;
        if (ctrl==0x12) {
            unsigned char alive[8]={0,0,0,2,0,0x13,0,HSFZ_TESTER};
            if (!hsfz_transfer(fd,alive,8,1,bd_mono_ms()+1000)) return -1;
            continue;
        }
        if (ctrl!=1 || len<3 || body[1]!=HSFZ_TESTER) continue;
        if (len>=5 && body[2]==0x7f && body[3]==uds[0] && body[4]==0x78) {
            idle = bd_mono_ms() + idle_ms + 2000; /* that unit is still working */
            continue;
        }
        if (!(body[2]==(unsigned char)(uds[0]+0x40) || (len>=4 && body[2]==0x7f && body[3]==uds[0])))
            continue;
        ++replies;
        callback(context, body[0], body+2, len-2);
        idle = bd_mono_ms() + idle_ms;
    }
    return replies;
}

void hsfz_session_init(hsfz_session *s, const char *host, unsigned port) {
    memset(s, 0, sizeof *s);
    s->host = host;
    s->port = port;
    s->fd = s->lock = -1;
}

int hsfz_session_fd(hsfz_session *s) {
    uint64_t now = bd_mono_ms();
    if (s->fd >= 0) return s->fd;
    if (now < s->retry_at) return -1;
    s->lock = reserve_gateway();
    if (s->lock < 0) return -1;
    s->fd = hsfz_connect(s->host, s->port);
    if (s->fd < 0) {
        release_gateway(&s->fd, &s->lock);
        s->retry_at = bd_mono_ms() + DIAGNOSTIC_RETRY_MS;
        return -1;
    }
    s->lease_until = bd_mono_ms() + DIAGNOSTIC_LEASE_MS;
    return s->fd;
}

void hsfz_session_done(hsfz_session *s, int transport_ok) {
    uint64_t now = bd_mono_ms();
    if (s->fd < 0) return;
    if (!transport_ok) {
        release_gateway(&s->fd, &s->lock);
        s->retry_at = now + DIAGNOSTIC_RETRY_MS;
    } else if (gateway_should_yield(s->lock, now, s->lease_until)) {
        release_gateway(&s->fd, &s->lock);
        s->retry_at = now + DIAGNOSTIC_YIELD_MS;
    }
}

void hsfz_session_close(hsfz_session *s) { release_gateway(&s->fd, &s->lock); }

void hsfz_text(unsigned model, const char *format, ...) {
    char line[400];
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    /* One update per line: never let a value break the protocol. */
    for (n = 0; line[n]; ++n) if (line[n] == '\n' || line[n] == '\r') line[n] = ' ';
    if (printf("T%u %s\n", model, line) < 0 || fflush(stdout) != 0) hsfz_running = 0;
}

static char shown_text[256][200];
static unsigned char shown_valid[256];
static unsigned shown_image[256];
static unsigned char shown_image_valid[256];

void hsfz_text_once(unsigned model, const char *format, ...) {
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
    hsfz_text(model, "%s", line);
}

void hsfz_image_once(unsigned model, unsigned image) {
    if (model < 256 && shown_image_valid[model] && shown_image[model] == image) return;
    if (model < 256) {
        shown_image[model] = image;
        shown_image_valid[model] = 1;
    }
    hsfz_image(model, image);
}

void hsfz_value_line(unsigned key, long number) {
    if (printf("V%u %ld\n", key, number) < 0 || fflush(stdout) != 0) hsfz_running = 0;
}

void hsfz_image(unsigned model, unsigned image) {
    if (printf("I%u %u\n", model, image) < 0 || fflush(stdout) != 0) hsfz_running = 0;
}

void hsfz_pause(unsigned ms) {
    uint64_t end = bd_mono_ms() + ms;
    while (hsfz_running) {
        uint64_t now = bd_mono_ms();
        if (now >= end) break;
        poll(0, 0, (int)(end - now));
    }
}

static int read_candidate(int fd, const hsfz_candidate *c, double *value) {
    unsigned char data[64];
    double raw = 0;
    unsigned i;
    int n = c->obd ? hsfz_obd(fd, c->ecu, c->id, data, sizeof data, 300)
                   : hsfz_did(fd, c->ecu, c->id, data, sizeof data, 300);
    if (n == -1) return -1;
    if (n < 0 || (unsigned)n < c->at + c->bytes || !c->bytes || c->bytes > 4) return 0;
    for (i = 0; i < c->bytes; ++i) raw = raw * 256 + data[c->at + i];
    if (c->is_signed && raw >= ldexp(1.0, 8 * (int)c->bytes - 1)) raw -= ldexp(1.0, 8 * (int)c->bytes);
    *value = raw * c->factor + c->offset;
    return *value >= c->min && *value <= c->max;
}

int hsfz_value(int fd, const hsfz_candidate *list, unsigned count, unsigned *pick,
               double *value) {
    unsigned k;
    for (k = 0; k < count; ++k) {
        unsigned i = (*pick + k) % count;
        int rc = read_candidate(fd, &list[i], value);
        if (rc < 0) return -1;
        if (rc) { *pick = i; return 1; }
    }
    return 0;
}
