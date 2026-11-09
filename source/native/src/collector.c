#define _POSIX_C_SOURCE 200809L
#include "gauge_core.h"
#include "diagnostic_lease.h"
#include "hsfz.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


#if defined(MHD_MAPS_ONLY)
int main(int argc, char **argv) {
    const char *host=0;
    unsigned port=6801, slot=0, negative;
    int fd, lock, n, i;
    unsigned char request[]={0x2f,0x01,0}, prefix[]={0x6f,0x01,0}, reply[16];
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--host") && i+1<argc && !host) host=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) port=hsfz_number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--map") && i+1<argc && !slot) slot=hsfz_number(argv[++i],1,4);
        else return 2;
    }
    if (!host || !slot) return 2;
    hsfz_signals();
    lock=reserve_gateway();
    if (lock<0) { puts("Diagnostics busy. Wait a moment and try again."); return 0; }
    fd=hsfz_connect(host,port);
    if (fd<0) {
        puts("Diagnostic gateway unavailable");
        close(lock);
        return 0;
    }
    /* MHD+ slot selector documented in idrive7-apps, tune.js at cca7f9a.
       The echoed value acknowledges the request but does not read active state. */
    request[2]=prefix[2]=(unsigned char)(slot-1);
    n=hsfz_exchange(fd,0x12,request,sizeof request,prefix,sizeof prefix,reply,sizeof reply,1600,&negative);
    if (n>=3) printf("Confirmed: Map %u\n",slot);
    else if (n==-2) printf("Map %u refused by DME (NRC %02X)\n",slot,negative);
    else puts("Map result unknown. Check Current Map in MHD.");
    close(fd);
    close(lock);
    return 0;
}
#elif defined(SHIFT_DECK_ONLY)
#include "drive_core.h"
#include "drive_frame.h"

/* The learned gear calibration (engine rpm per km/h at a 1.000 ratio). */
static double load_calibration(const char *path) {
    FILE *f = path ? fopen(path, "r") : 0;
    double factor = 0;
    if (!f) return 0;
    if (fscanf(f, "%lf", &factor) != 1) factor = 0;
    fclose(f);
    return factor;
}

static void save_calibration(const char *path, double factor) {
    char temporary[256];
    FILE *f;
    if (!path || snprintf(temporary, sizeof temporary, "%s.new", path) >= (int)sizeof temporary) return;
    f = fopen(temporary, "w");
    if (!f) return;
    fprintf(f, "%.4f\n", factor);
    if (fclose(f) == 0) rename(temporary, path);
    else remove(temporary);
}

static int read_live_pid(int fd, unsigned char pid, int width, int *value) {
    unsigned char req[]={0x01,pid}, prefix[]={0x41,pid}, reply[4];
    unsigned negative;
    int n=hsfz_exchange(fd,0x12,req,sizeof req,prefix,sizeof prefix,reply,sizeof reply,250,&negative);
    *value=-1;
    if (n==width+2) *value=width==1 ? reply[2] : ((unsigned)reply[2]*256+reply[3])/4;
    return n!=-1;
}

int main(int argc, char **argv) {
    const char *host=0, *calibration=0;
    unsigned port=6801, udp_port=DRIVE_PORT, cycles=0, count=0;
    pid_t parent=getppid();
    struct sockaddr_in dest;
    int fd=-1, sock, lock=-1, i;
    uint64_t lease_until=0, retry_at=0;
    gear_state gears;
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--host") && i+1<argc && !host) host=argv[++i];
        else if (!strcmp(argv[i],"--calibration") && i+1<argc) calibration=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) port=hsfz_number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--udp-port") && i+1<argc) udp_port=hsfz_number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--cycles") && i+1<argc) cycles=hsfz_number(argv[++i],1,1000);
        else return 2;
    }
    if (!host || parent<=1) return 2;
    hsfz_signals();
    gear_init(&gears, load_calibration(calibration));
    sock=socket(AF_INET,SOCK_DGRAM,0);
    if (sock<0) return 1;
    memset(&dest,0,sizeof dest);
    dest.sin_family=AF_INET;
    dest.sin_port=htons((uint16_t)udp_port);
    inet_pton(AF_INET,"127.0.0.1",&dest.sin_addr);
    while (hsfz_running && getppid()==parent && (!cycles || count<cycles)) {
        struct drive_frame f;
        int rpm=-1,kph=-1;
        uint64_t start=bd_mono_ms(), elapsed;
        if (fd<0 && start>=retry_at) {
            lock=reserve_gateway();
            if (lock>=0) {
                fd=hsfz_connect(host,port);
                if (fd<0) {
                    release_gateway(&fd,&lock);
                    retry_at=bd_mono_ms()+DIAGNOSTIC_RETRY_MS;
                } else lease_until=bd_mono_ms()+DIAGNOSTIC_LEASE_MS;
            }
        }
        if (fd>=0) {
            if (!read_live_pid(fd,0x0c,2,&rpm) || !read_live_pid(fd,0x0d,1,&kph)) {
                release_gateway(&fd,&lock);
                retry_at=bd_mono_ms()+DIAGNOSTIC_RETRY_MS;
            } else {
                int learned=0;
                f.sampled_ms=bd_mono_ms();
                f.rpm=rpm;
                f.kph=kph;
                f.gear=gear_update(&gears,rpm,kph,&learned);
                f.reserved=0;
                if (learned) save_calibration(calibration,gears.factor);
                if (sendto(sock,&f,sizeof f,0,(struct sockaddr *)&dest,sizeof dest)!=(ssize_t)sizeof f) break;
                if (gateway_should_yield(lock,f.sampled_ms,lease_until)) {
                    release_gateway(&fd,&lock);
                    retry_at=f.sampled_ms+DIAGNOSTIC_YIELD_MS;
                }
            }
        }
        ++count;
        elapsed=bd_mono_ms()-start;
        if (hsfz_running) poll(0,0,elapsed<70 ? (int)(70-elapsed) : 10);
    }
    release_gateway(&fd,&lock);
    close(sock);
    return 0;
}
#else
static int trace_reads = 0;
static void pause_ms(int ms) { while (hsfz_running && poll(0, 0, ms) < 0 && errno == EINTR) {} }

static int read_did(int fd, unsigned ecu, unsigned did, double *value) {
    unsigned char request[]={0x22,(unsigned char)(did>>8),(unsigned char)did};
    unsigned char prefix[]={0x62,request[1],request[2]};
    unsigned char reply[4094];
    unsigned negative, i;
    int n=hsfz_exchange(fd,ecu,request,3,prefix,3,reply,sizeof reply,500,&negative);
    int decoded;
    if (n==-2) {
        fprintf(stderr,"ECU %02X DID %04X NRC %02X\n",ecu,did,negative);
        return 0;
    }
    if (n<0) return -1;
    decoded=bd_decode_did(did,reply+3,(size_t)n-3,value);
    if (trace_reads) {
        fprintf(stderr,"RAW mono_ms=%llu ecu=%02X did=%04X payload=",
                (unsigned long long)bd_mono_ms(),ecu,did);
        for (i=3;i<(unsigned)n;++i) fprintf(stderr,"%02X",reply[i]);
        if (decoded) fprintf(stderr," decoded=%.6f\n",*value);
        else fprintf(stderr," unavailable%s\n",did==0x4a49 && n==5 &&
                     reply[3]==255 && reply[4]==255 ?
                     " reason=ambiguous-FFFF" : " reason=invalid-or-out-of-range");
    }
    return decoded;
}

static int send_frame(int sock, struct sockaddr_in *dest, bd_frame *f, uint64_t *observed) {
    char packet[BD_MAX_PACKET];
    uint64_t now = bd_mono_ms();
    unsigned i;
    int len;
    f->sent_ms=now;
    for (i = 0; i < BD_COUNT; ++i) {
        uint64_t age = now - observed[i];
        f->age_ms[i] = age > 999999 ? 999999 : (unsigned)age;
    }
    len = bd_encode(f, packet, sizeof packet);
    return len > 0 && sendto(sock, packet, (size_t)len, 0,
        (struct sockaddr *)dest, sizeof *dest) == len;
}
int vitals_main(int argc, char **argv);

int main(int argc, char **argv) {
    const char *host = 0;
    unsigned port = 6801, udp_port = BD_UDP_PORT, cycles = 0, count = 0, i;
    for (i = 1; i < (unsigned)argc; ++i)
        if (!strcmp(argv[i], "--vitals")) return vitals_main(argc, argv);
    int demo = 0, sock, gw = -1, no_trans = 0, session = 0;
    int lock = -1;
    pid_t parent = getppid();
    struct sockaddr_in dest;
    bd_frame f;
    uint64_t observed[BD_COUNT] = {0};
    double ambient = 0;
    uint64_t ambient_time = 0;
    int have_ambient = 0;
    const unsigned tick_ms = 150;
    const unsigned minimum_idle_ms = 10;
    /* Each slow signal has its own deadline. Stagger temperature reads
     * after the first scan so they do not delay the fast gauges together. */
    const unsigned dids[] = {0x4201,0x4205,0x5889,0x4a49,0x580f,0xda12,0x56d7,0x586f,0x4300};
    const unsigned periods[] = {1000,0,0,0,2000,2000,0,0,2000};
    const unsigned first_delay[] = {900,0,0,0,500,1150,0,0,1700};
    uint64_t due[sizeof dids/sizeof dids[0]] = {0};
    uint64_t failed_until[sizeof dids/sizeof dids[0]] = {0};
    uint64_t lease_until=0, retry_at=0;
    int scanned=0;
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--session")) session = 1;
        else if (!strcmp(argv[i], "--trace")) trace_reads = 1;
        else if (!strcmp(argv[i], "--no-trans")) no_trans = 1;
        else if (!strcmp(argv[i], "--host") && i+1 < (unsigned)argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1 < (unsigned)argc) port = hsfz_number(argv[++i],1,65535);
        else if (!strcmp(argv[i], "--udp-port") && i+1 < (unsigned)argc) udp_port = hsfz_number(argv[++i],1,65535);
        else if (!strcmp(argv[i], "--cycles") && i+1 < (unsigned)argc) cycles = hsfz_number(argv[++i],1,1000000);
        else { fprintf(stderr, "Usage: %s (--host IPv4 | --demo) [--port 6801] [--udp-port 42860] [--cycles N] [--no-trans] [--trace] [--session]\n", argv[0]); return 2; }
    }
    if ((!host && !demo) || (host && demo)) { fprintf(stderr, "Choose --host or --demo, never both.\n"); return 2; }
    hsfz_signals();
    if (session && (parent <= 1 || demo)) return 2;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("UDP socket"); return 1; }
    memset(&dest,0,sizeof dest); dest.sin_family=AF_INET; dest.sin_port=htons((uint16_t)udp_port);
    inet_pton(AF_INET,"127.0.0.1",&dest.sin_addr); /* local process IPC only */
    memset(&f,0,sizeof f); f.demo=(unsigned)demo;
    fprintf(stderr, "%s: 2016 340i candidate profile; validate against ISTA/logger.\n",
        demo ? "DEMO / NO VEHICLE CONNECTION" : "READ ONLY / HSFZ");
    while (hsfz_running && (!cycles || count < cycles) && (!session || getppid() == parent)) {
        uint64_t start = bd_mono_ms();
        f.seq++;
        if (demo) {
            double t = count * (tick_ms / 1000.0);
            f.value[0]=12+8*sin(t); f.value[1]=13.2+.6*sin(t);
            f.value[2]=8+3*sin(t); f.value[3]=44+2*sin(t); f.value[4]=88;
            f.value[BD_RAIL]=95+22*sin(t); f.value[BD_OIL]=2.5+.5*sin(t);
            f.value[BD_COOLANT]=86+3*sin(t);
            f.valid=(1u<<BD_COUNT)-1;
            for (i=0;i<BD_COUNT;++i) observed[i]=bd_mono_ms();
        } else {
            int full_scan = session ? !scanned : gw < 0;
            if (gw < 0 && start >= retry_at) {
                if (session) lock=reserve_gateway();
                if (!session || lock>=0) {
                    gw=hsfz_connect(host,port);
                    if (gw<0 && session) {
                        release_gateway(&gw,&lock);
                        retry_at=bd_mono_ms()+DIAGNOSTIC_RETRY_MS;
                    } else if (session) lease_until=bd_mono_ms()+DIAGNOSTIC_LEASE_MS;
                }
            }
            for (i=0; gw >= 0 && i < sizeof dids / sizeof dids[0] && hsfz_running; ++i) {
                double v = 0;
                int rc;
                if (no_trans && dids[i] == 0xda12) continue;
                if (session && start < failed_until[i]) continue;
                if (!full_scan && periods[i] && start < due[i]) continue;
                if (full_scan) due[i]=start+first_delay[i];
                else if (periods[i]) {
                    due[i]+=periods[i];
                    if (due[i]<=start) due[i]=start+periods[i];
                }
                if (i != 0) f.valid &= ~(1u << (i-1));
                rc = read_did(gw, dids[i] == 0xda12 ? 0x18 : 0x12, dids[i], &v);
                if (rc < 0) {
                    release_gateway(&gw,&lock);
                    if (session) {
                        failed_until[i]=bd_mono_ms()+2000;
                        retry_at=bd_mono_ms()+DIAGNOSTIC_RETRY_MS;
                    } else {
                        f.valid=0;
                        have_ambient=0;
                    }
                    break;
                }
                if (i == 0) {
                    have_ambient=rc != 0;
                    if (have_ambient) { ambient=v; ambient_time=bd_mono_ms(); }
                    continue;
                }
                if (!rc) continue;
                if (i == 1) {
                    if (!have_ambient || bd_mono_ms()-ambient_time > 1800) continue;
                    v = (v-ambient) * .145037738; /* kPa absolute - measured ambient -> gauge psi */
                }
                if (i == 2) v *= 14.7; /* measured lambda -> gasoline-equivalent AFR */
                f.value[i-1]=v; observed[i-1]=bd_mono_ms(); f.valid |= 1u << (i-1);
            }
            if (gw>=0) {
                scanned=1;
                if (session && gateway_should_yield(lock,bd_mono_ms(),lease_until)) {
                    release_gateway(&gw,&lock);
                    retry_at=bd_mono_ms()+DIAGNOSTIC_YIELD_MS;
                }
            }
        }
        if (!send_frame(sock,&dest,&f,observed)) { perror("send frame"); hsfz_running=0; }
        ++count;
        {
            uint64_t elapsed = bd_mono_ms()-start;
            unsigned interval = session && gw<0 ? 55 : tick_ms;
            unsigned idle_ms = elapsed < interval ? (unsigned)(interval-elapsed) : 0;
            if (!demo && idle_ms < minimum_idle_ms) idle_ms = minimum_idle_ms;
            if (idle_ms) pause_ms((int)idle_ms);
        }
    }
    release_gateway(&gw,&lock);
    close(sock);
    return 0;
}
#endif
