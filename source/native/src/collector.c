#define _POSIX_C_SOURCE 200809L
#include "gauge_core.h"
#include "diagnostic_lease.h"
#include "diag_link.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned number(const char *s, unsigned lo, unsigned hi) {
    char *end; unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n < lo || n > hi) { fprintf(stderr, "Invalid number: %s\n", s); exit(2); }
    return (unsigned)n;
}

#if defined(ENGINE_CODES_ONLY) || defined(MHD_MAPS_ONLY)
#include <fcntl.h>

/* The result is out: end the plugin's pipe now, before the link's bounded close. */
static void result_done(void) {
    int null;
    fflush(stdout);
    null = open("/dev/null", O_WRONLY);
    if (null >= 0) { dup2(null, 1); close(null); }
}
#endif

#if defined(BM3_ONLY) || defined(XDRIVE_ONLY)
/* These controls write to the DME and DSC and read ECUs never seen through the MOST tunnel, so they stay
 * on HSFZ. */
static void stop(int sig) { (void)sig; diag_running = 0; }
static int connect_gw(const char *host, unsigned port) { return diag_hsfz_connect(host, port); }
static int exchange(int fd, unsigned ecu, const unsigned char *uds, unsigned uds_size,
                    const unsigned char *prefix, unsigned prefix_size,
                    unsigned char *reply, unsigned reply_size, unsigned timeout_ms,
                    unsigned *negative) {
    return diag_hsfz_exchange(fd, ecu, uds, uds_size, prefix, prefix_size, reply, reply_size,
                              timeout_ms, negative);
}
#include <ctype.h>
#include "controls.c"
#elif defined(ENGINE_CODES_ONLY)
#include "most/most_link.h"

static void fault_codes(struct diag_link *link, int clear) {
    /* Fault, pending, confirmed, failed since clear, or warning requested.
     * Bits 0x10 and 0x40 only report incomplete tests. */
    enum { fault_status = 0xaf };
    const unsigned char read_request[]={0x19,0x02,fault_status}, read_reply[]={0x59,0x02};
    const unsigned char clear_request[]={0x14,0xff,0xff,0xff}, clear_reply[]={0x54};
    unsigned char reply[4094];
    unsigned negative;
    unsigned supported_fault_status;
    int n, i, count=0, shown=0, full;
    if (!diag_link_up(link)) { puts("Diagnostic gateway unavailable"); return; }
    if (clear) {
        n=diag_link_request(link,0x12,clear_request,4,clear_reply,1,reply,sizeof reply,8000,&negative);
        // Retry a clear only after a session-related refusal and a positive session reply.
        if (n==-2 && (negative==0x22 || negative==0x31 || negative==0x7e || negative==0x7f)) {
            const unsigned char extended[]={0x10,0x03}, extended_reply[]={0x50,0x03};
            const unsigned char normal[]={0x10,0x01}, normal_reply[]={0x50,0x01};
            unsigned session_nrc;
            if (diag_link_request(link,0x12,extended,2,extended_reply,2,reply,sizeof reply,2000,&session_nrc)>=2) {
                n=diag_link_request(link,0x12,clear_request,4,clear_reply,1,reply,sizeof reply,8000,&negative);
                diag_link_request(link,0x12,normal,2,normal_reply,2,reply,sizeof reply,2000,&session_nrc);
            }
        }
        if (n!=1) {
            if (n==-2 && (negative==0x22 || negative==0x83 || negative==0x88))
                printf("Clear refused (NRC %02X). Try ignition on, engine off.\n",negative);
            else if (n==-2) printf("Clear rejected (NRC %02X)\n",negative);
            else puts("Clear result unknown: read codes again");
            return;
        }
    }
    n=diag_link_request(link,0x12,read_request,3,read_reply,2,reply,sizeof reply,1800,&negative);
    if (n==-2) { printf("Read rejected (NRC %02X)\n",negative); return; }
    full = diag_link_on_most(link) && n==MOST_INNER_MAX;   /* the tunnel reply filled its cap */
    if (full) n-=(n-3)%4;   /* it may end mid-record */
    if (n<3 || (n-3)%4!=0) { puts(clear ? "Clear acknowledged; recheck unavailable" : "No valid code response"); return; }
    supported_fault_status=reply[2] & fault_status;
    for (i=3;i<n;i+=4)
        if (reply[i+3] & supported_fault_status) ++count;
    printf("Engine: %d %s%s\n",count,
           count==1 ? "code" : "codes",clear ? " after clear" : "");
    for (i=3;i<n && shown<128;i+=4) {
        if (!(reply[i+3] & supported_fault_status)) continue;
        printf("%02X%02X%02X  status %02X\n",reply[i],reply[i+1],reply[i+2],reply[i+3]);
        ++shown;
    }
    if (count>128) puts("More codes returned; first 128 shown");
    if (full) puts("More codes may exist");
}

int main(int argc, char **argv) {
    const char *host=0;
    unsigned port=6801;
    int mode=0, transport=DIAG_AUTO, i;
    struct diag_link link;
    struct diag_link_cfg cfg;
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--host") && i+1<argc && !host) host=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) port=number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"auto")) ++i;
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"hsfz")) { transport=DIAG_HSFZ; ++i; }
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"most")) { transport=DIAG_MOST; ++i; }
        else if (!strcmp(argv[i],"--codes") && !mode) mode=1;
        else if (!strcmp(argv[i],"--clear") && !mode) mode=2;
        else break;
    }
    if (i!=argc || !mode || (transport==DIAG_HSFZ && !host)) {
        fprintf(stderr,"Usage: %s [--host IPv4] [--transport auto|hsfz|most] [--port 6801] (--codes | --clear)\n",argv[0]);
        return 2;
    }
    diag_link_signals();
    memset(&cfg,0,sizeof cfg);
    cfg.host=host; cfg.port=port; cfg.transport=transport; cfg.kind=DIAG_ONESHOT; cfg.dsi_name="enginecodes";
    diag_link_init(&link,&cfg);
    if (!diag_link_reserve_oneshot(&link)) {
        puts(errno==EADDRINUSE ? "Diagnostics busy. Wait a moment and try again."
                               : "Could not reserve the diagnostics connection");
        return 0;
    }
    fault_codes(&link,mode==2);
    result_done();
    diag_link_close(&link);
    return 0;
}
#elif defined(MHD_MAPS_ONLY)
int main(int argc, char **argv) {
    const char *host=0;
    unsigned port=6801, function=0, value=0, negative;
    int n, i, transport=DIAG_AUTO;
    struct diag_link link;
    struct diag_link_cfg cfg;
    unsigned char request[]={0x2f,0,0}, prefix[]={0x6f,0,0}, reply[16];
    const char *label = 0;
#ifdef PERF_LAB
    const char *flap[] = {"Exhaust OEM", "Exhaust open", "Exhaust closed"};
#endif
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--host") && i+1<argc && !host) host=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) port=number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"auto")) ++i;
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"hsfz")) { transport=DIAG_HSFZ; ++i; }
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"most")) { transport=DIAG_MOST; ++i; }
        else if (!strcmp(argv[i],"--map") && i+1<argc && !function) {
            function=1; value=number(argv[++i],1,4)-1;
        }
#ifdef PERF_LAB
        else if (!strcmp(argv[i],"--flap") && i+1<argc && !function) {
            function=2; value=number(argv[++i],0,2); label=flap[value];
        } else if (!strcmp(argv[i],"--cooling") && i+1<argc && !function) {
            function=3; value=number(argv[++i],0,1); label=value?"Max Cooling on":"Max Cooling off";
        }
#endif
        else return 2;
    }
    if (!function || (transport==DIAG_HSFZ && !host)) return 2;
    diag_link_signals();
    memset(&cfg,0,sizeof cfg);
    cfg.host=host; cfg.port=port; cfg.transport=transport; cfg.kind=DIAG_ONESHOT; cfg.dsi_name="mhdmaps";
    diag_link_init(&link,&cfg);
    if (!diag_link_reserve_oneshot(&link)) { puts("Diagnostics busy. Wait a moment and try again."); return 0; }
    if (!diag_link_up(&link)) {
        puts("Diagnostic gateway unavailable");
        result_done();
        diag_link_close(&link);
        return 0;
    }
    /* MHD+ live controls documented in idrive7-apps at 9f0f68a.
       The matching echo acknowledges the request. */
#ifdef PERF_LAB
    if (function==3 && value) {
        const unsigned char rpm[]={0x01,0x0c}, rpm_reply[]={0x41,0x0c};
        n=diag_link_request(&link,0x12,rpm,sizeof rpm,rpm_reply,sizeof rpm_reply,reply,sizeof reply,500,&negative);
        if (n!=4 || (((unsigned)reply[2]<<8)|reply[3])<=2000 ||
            (((unsigned)reply[2]<<8)|reply[3])>40000) {
            puts("Start the engine first. Max Cooling ON needs live RPM.");
            result_done();
            diag_link_close(&link);
            return 0;
        }
    }
#endif
    request[1]=prefix[1]=(unsigned char)function;
    request[2]=prefix[2]=(unsigned char)value;
    n=diag_link_request(&link,0x12,request,sizeof request,prefix,sizeof prefix,reply,sizeof reply,1600,&negative);
    if (label) {
        if (n>=3) printf("Accepted by DME: %s\n",label);
        else if (n==-2) printf("%s refused by DME (NRC %02X)\n",label,negative);
        else printf("%s: no matching DME reply\n",label);
    } else if (n>=3) printf("Confirmed: Map %u\n",value+1);
    else if (n==-2) printf("Map %u refused by DME (NRC %02X)\n",value+1,negative);
    else puts("Map result unknown. Check Current Map in MHD.");
    result_done();
    diag_link_close(&link);
    return 0;
}
#elif defined(SHIFT_DECK_ONLY)
#include "drive_core.h"
#include "drive_frame.h"

/* The learned gear calibration (engine rpm per km/h at a 1.000 ratio), 0 when none is saved. */
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

/* The raw PID value, -1 when the ECU refused it. Returns 0 read, -1 link lost, -3 skip this frame. */
static int read_live_pid(struct diag_link *link, unsigned char pid, int width, int *value) {
    unsigned char req[]={0x01,pid}, prefix[]={0x41,pid}, reply[4];
    unsigned negative;
    int n=diag_link_request(link,0x12,req,sizeof req,prefix,sizeof prefix,reply,sizeof reply,250,&negative);
    *value=-1;
    if (n==-1 || n==-3) return n;
    if (n==width+2) *value=width==1 ? reply[2] : ((unsigned)reply[2]*256+reply[3])/4;
    return 0;
}

int main(int argc, char **argv) {
    struct diag_link_cfg cfg={0,6801,DIAG_AUTO,DIAG_LOOP,"shiftdeck",1};
    struct diag_link link;
    const char *calibration=0;
    unsigned udp_port=DRIVE_PORT, cycles=0, count=0;
    pid_t parent=getppid();
    struct sockaddr_in dest;
    int sock, i, lost;
    gear_state gears;
#ifdef NATIVE_FAST
    uint64_t speed_due=0;
    int last_speed=-1;
#endif
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--host") && i+1<argc && !cfg.host) cfg.host=argv[++i];
        else if (!strcmp(argv[i],"--port") && i+1<argc) cfg.port=number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--udp-port") && i+1<argc) udp_port=number(argv[++i],1,65535);
        else if (!strcmp(argv[i],"--cycles") && i+1<argc) cycles=number(argv[++i],1,1000);
        else if (!strcmp(argv[i],"--calibration") && i+1<argc) calibration=argv[++i];
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"auto")) ++i;
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"hsfz")) { cfg.transport=DIAG_HSFZ; ++i; }
        else if (!strcmp(argv[i],"--transport") && i+1<argc && !strcmp(argv[i+1],"most")) { cfg.transport=DIAG_MOST; ++i; }
        else return 2;
    }
    if ((!cfg.host && cfg.transport==DIAG_HSFZ) || parent<=1) return 2;
    diag_link_signals();
    diag_link_init(&link,&cfg);
    gear_init(&gears,load_calibration(calibration));
    sock=socket(AF_INET,SOCK_DGRAM,0);
    if (sock<0) return 1;
    memset(&dest,0,sizeof dest);
    dest.sin_family=AF_INET;
    dest.sin_port=htons((uint16_t)udp_port);
    inet_pton(AF_INET,"127.0.0.1",&dest.sin_addr);
    while (diag_running && getppid()==parent && (!cycles || count<cycles) && !diag_link_lost(&link)) {
        struct drive_frame f;
        int rpm=-1,kph=-1,rc;
        uint64_t start=bd_mono_ms(), elapsed;
        int up=diag_link_up(&link);
        if (up) {
            rc=read_live_pid(&link,0x0c,2,&rpm);
#ifdef NATIVE_FAST
            if (!rc && start>=speed_due) {
                rc=read_live_pid(&link,0x0d,1,&last_speed);
                speed_due=start+65;
            }
            kph=last_speed;
#else
            if (!rc) rc=read_live_pid(&link,0x0d,1,&kph);
#endif
            if (rc==-1) diag_link_drop(&link);
            else if (!rc) {
                int learned=0;
                f.sampled_ms=bd_mono_ms();
                f.rpm=rpm;
                f.kph=kph;
                f.gear=gear_update(&gears,rpm,kph,&learned);
                f.reserved=0;
                if (learned) save_calibration(calibration,gears.factor);
                if (sendto(sock,&f,sizeof f,0,(struct sockaddr *)&dest,sizeof dest)!=(ssize_t)sizeof f) break;
                diag_link_should_yield(&link,f.sampled_ms);
            }
        }
        diag_link_tick(&link);
        ++count;
        elapsed=bd_mono_ms()-start;
#ifdef NATIVE_FAST
        if (diag_running) poll(0,0,!up ? 25 : elapsed<20 ? (int)(20-elapsed) : 2);
#else
        if (diag_running) poll(0,0,elapsed<70 ? (int)(70-elapsed) : 10);
#endif
    }
    lost=diag_link_lost(&link);
    diag_link_close(&link);
    close(sock);
    return lost ? 1 : 0;
}
#else
#include "most/dsi_client.h"
#include "most/most_link.h"

static int trace_reads = 0;
static void pause_ms(int ms) { while (diag_running && poll(0, 0, ms) < 0 && errno == EINTR) {} }

static int decode_reply(unsigned ecu, unsigned did, const unsigned char *reply, int n, double *value) {
    int decoded=bd_decode_did(did,reply+3,(size_t)n-3,value);
    int i;
    if (trace_reads) {
        fprintf(stderr,"RAW mono_ms=%llu ecu=%02X did=%04X payload=",
                (unsigned long long)bd_mono_ms(),ecu,did);
        for (i=3;i<n;++i) fprintf(stderr,"%02X",reply[i]);
        if (decoded) fprintf(stderr," decoded=%.6f\n",*value);
        else fprintf(stderr," unavailable%s\n",did==0x4a49 && n==5 &&
                     reply[3]==255 && reply[4]==255 ?
                     " reason=ambiguous-FFFF" : " reason=invalid-or-out-of-range");
    }
    return decoded;
}

/* 1 decoded, 0 unavailable or refused, -1 link lost, -3 this read failed but the link is kept. */
static int read_did(struct diag_link *link, unsigned ecu, unsigned did, double *value) {
    unsigned char request[]={0x22,(unsigned char)(did>>8),(unsigned char)did};
    unsigned char prefix[]={0x62,request[1],request[2]};
    unsigned char reply[4094];
    unsigned negative;
    int n=diag_link_request(link,ecu,request,3,prefix,3,reply,
                            diag_link_on_most(link) ? 64 : sizeof reply,500,&negative);
    if (n==-2) {
        fprintf(stderr,"ECU %02X DID %04X NRC %02X\n",ecu,did,negative);
        return 0;
    }
    if (n==-3) return -3;
    if (n<0) return -1;
    return decode_reply(ecu,did,reply,n,value);
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
/* BoostDeck's More vitals page runs this worker with --vitals (vitals.c). */
int vitals_main(int argc, char **argv);

int main(int argc, char **argv) {
    const char *host = 0;
    unsigned port = 6801, udp_port = BD_UDP_PORT, cycles = 0, count = 0, i;
    int demo = 0, sock, no_trans = 0, session = 0, scanned = 0, transport = DIAG_AUTO, most_lost = 0;
    struct diag_link link;
    struct diag_link_cfg cfg;
    pid_t parent = getppid();
    struct sockaddr_in dest;
    bd_frame f;
    uint64_t observed[BD_COUNT] = {0};
    double ambient = 0;
    uint64_t ambient_time = 0;
    int have_ambient = 0;
#ifdef NATIVE_FAST
    const unsigned tick_ms = 35;
    const unsigned minimum_idle_ms = 3;
#else
    const unsigned tick_ms = 150;
    const unsigned minimum_idle_ms = 10;
#endif
    /* Each slow signal has its own deadline. Stagger temperature reads
     * after the first scan so they do not delay the fast gauges together. */
    const unsigned dids[] = {0x4201,0x4205,0x5889,0x4a49,0x580f,0xda12,0x56d7,0x586f,0x4300};
    const unsigned periods[] = {1000,0,0,0,2000,2000,0,0,2000};
    const unsigned first_delay[] = {900,0,0,0,500,1150,0,0,1700};
    uint64_t due[sizeof dids/sizeof dids[0]] = {0};
    uint64_t failed_until[sizeof dids/sizeof dids[0]] = {0};
#ifdef PERF_LAB
    int ethanol=0;
    uint64_t next_ethanol=0;
#endif
    for (i = 1; i < (unsigned)argc; ++i)
        if (!strcmp(argv[i], "--vitals")) return vitals_main(argc, argv);
    for (i = 1; i < (unsigned)argc; ++i) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
#ifdef PERF_LAB
        else if (!strcmp(argv[i], "--ethanol")) ethanol = 1;
#endif
        else if (!strcmp(argv[i], "--session")) session = 1;
        else if (!strcmp(argv[i], "--trace")) trace_reads = 1;
        else if (!strcmp(argv[i], "--no-trans")) no_trans = 1;
        else if (!strcmp(argv[i], "--host") && i+1 < (unsigned)argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1 < (unsigned)argc) port = number(argv[++i],1,65535);
        else if (!strcmp(argv[i], "--udp-port") && i+1 < (unsigned)argc) udp_port = number(argv[++i],1,65535);
        else if (!strcmp(argv[i], "--cycles") && i+1 < (unsigned)argc) cycles = number(argv[++i],1,1000000);
        else if (!strcmp(argv[i], "--transport") && i+1 < (unsigned)argc && !strcmp(argv[i+1], "auto")) ++i;
        else if (!strcmp(argv[i], "--transport") && i+1 < (unsigned)argc && !strcmp(argv[i+1], "hsfz")) { transport = DIAG_HSFZ; ++i; }
        else if (!strcmp(argv[i], "--transport") && i+1 < (unsigned)argc && !strcmp(argv[i+1], "most")) { transport = DIAG_MOST; ++i; }
        else { fprintf(stderr, "Usage: %s (--host IPv4 | --demo) [--transport auto|hsfz|most] [--port 6801] [--udp-port 42860] [--cycles N] [--no-trans] [--trace] [--session]\n", argv[0]); return 2; }
    }
    if (host && demo) { fprintf(stderr, "Choose --host or --demo, never both.\n"); return 2; }
    if (!host && !demo && transport == DIAG_HSFZ) { fprintf(stderr, "--transport hsfz needs --host.\n"); return 2; }
    if (transport == DIAG_MOST) host = 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.host = host; cfg.port = port; cfg.transport = transport; cfg.kind = DIAG_LOOP;
    cfg.dsi_name = "boostdeck"; cfg.session = session;
    diag_link_init(&link, &cfg);
    diag_link_signals();
    if (trace_reads) most_log_level(MOST_LOG_I);
    if (session && (parent <= 1 || demo)) return 2;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("UDP socket"); return 1; }
    memset(&dest,0,sizeof dest); dest.sin_family=AF_INET; dest.sin_port=htons((uint16_t)udp_port);
    inet_pton(AF_INET,"127.0.0.1",&dest.sin_addr); /* local process IPC only */
    memset(&f,0,sizeof f); f.demo=(unsigned)demo;
    fprintf(stderr, "%s: 2016 340i candidate profile; validate against ISTA/logger.\n",
        demo ? "DEMO / NO VEHICLE CONNECTION" : host ? "READ ONLY / HSFZ" : "READ ONLY / MOST");
    while (diag_running && (!cycles || count < cycles) && (!session || getppid() == parent)) {
        uint64_t start = bd_mono_ms();
        f.seq++;
        if (demo) {
            double t = count * (tick_ms / 1000.0);
            f.value[0]=12+8*sin(t); f.value[1]=13.2+.6*sin(t);
            f.value[2]=8+3*sin(t); f.value[3]=44+2*sin(t); f.value[4]=88;
            f.value[BD_RAIL]=95+22*sin(t); f.value[BD_OIL]=2.5+.5*sin(t);
            f.value[BD_COOLANT]=86+3*sin(t);
#ifdef PERF_LAB
            f.value[BD_ETHANOL]=38;
#endif
            f.valid=(1u<<BD_COUNT)-1;
            for (i=0;i<BD_COUNT;++i) observed[i]=bd_mono_ms();
        } else {
            int full_scan = session ? !scanned : link.fd < 0 && !diag_link_on_most(&link);
            int up;
            diag_link_tick(&link);
            up = diag_link_up(&link);
            if (diag_link_on_most(&link)) f.valid &= ~(1u << BD_TRANS); /* no EGS reads on MOST */
            for (i=0; up && i < sizeof dids / sizeof dids[0] && diag_running; ++i) {
                double v = 0;
                int rc;
                unsigned kept;
                int on_most = diag_link_on_most(&link);
                if ((no_trans || on_most) && dids[i] == 0xda12) continue;
                if ((session || on_most) && start < failed_until[i]) continue;
                if (!full_scan && periods[i] && start < due[i]) continue;
                if (full_scan) due[i]=start+first_delay[i];
                else if (periods[i]) {
                    due[i]+=periods[i];
                    if (due[i]<=start) due[i]=start+periods[i];
                }
                kept = i != 0 ? f.valid & (1u << (i-1)) : 0;
                f.valid &= ~kept;
                rc = read_did(&link, dids[i] == 0xda12 ? 0x18 : 0x12, dids[i], &v);
                if (rc == -3) { /* skip it for a while; a value still fresh stays shown */
                    failed_until[i] = bd_mono_ms()+2000;
                    if (kept) {
                        uint64_t age = bd_mono_ms()-observed[i-1];
                        f.age_ms[i-1] = age > 999999 ? 999999 : (unsigned)age;
                        f.valid |= kept;
                        if (!bd_is_fresh(&f, i-1, 0)) f.valid &= ~kept;
                    }
                    break;
                }
                if (rc < 0) {
                    diag_link_drop(&link);
                    if (diag_link_lost(&link)) { most_lost = 1; diag_running = 0; }
                    else if (session) failed_until[i]=bd_mono_ms()+2000;
                    else {
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
                /* A tunnel read is slow, so each new value goes out at once. */
                if (diag_link_on_most(&link) && !send_frame(sock,&dest,&f,observed)) { perror("send frame"); diag_running=0; }
            }
#ifdef PERF_LAB
            /* Not over MOST: the tunnel's echo check and 2 s wait do not suit this read. */
            if (up && link.fd>=0 && ethanol && start>=next_ethanol && diag_running) {
                /* Published MHD+ MG1 monitor layout. This is the tune's active
                 * ethanol percentage, distinct from the raw ECA sensor. */
                const unsigned char query[]={0x2c,0x88,0x01}, prefix[]={0x6c,0x88};
                unsigned char reply[38];
                unsigned negative;
                const int n=diag_link_request(&link,0x12,query,sizeof query,prefix,sizeof prefix,
                                              reply,sizeof reply,180,&negative);
                f.valid &= ~(1u<<BD_ETHANOL);
                if (n==38 && reply[17]<=100) {
                    observed[BD_ETHANOL]=bd_mono_ms();
                    f.value[BD_ETHANOL]=reply[17];
                    f.valid |= 1u<<BD_ETHANOL;
                    next_ethanol=observed[BD_ETHANOL]+5000;
                } else {
                    next_ethanol=bd_mono_ms()+30000;
                    if (n==-1) diag_link_drop(&link);
                }
            }
#endif
            if (diag_link_on_most(&link)) scanned=1;
            else if (link.fd>=0) {
                scanned=1;
                (void)diag_link_should_yield(&link,bd_mono_ms());
            }
        }
        if (!send_frame(sock,&dest,&f,observed)) { perror("send frame"); diag_running=0; }
        ++count;
        {
            uint64_t elapsed = bd_mono_ms()-start;
            unsigned interval = session && link.fd<0 && !diag_link_on_most(&link) ? 55 : tick_ms;
            unsigned idle_ms = elapsed < interval ? (unsigned)(interval-elapsed) : 0;
            if (!demo && idle_ms < minimum_idle_ms) idle_ms = minimum_idle_ms;
            if (idle_ms) pause_ms((int)idle_ms);
        }
    }
    diag_link_close(&link);
    close(sock);
    return most_lost ? 1 : 0;
}
#endif
