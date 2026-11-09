// Drive each plugin through the mock framework with a scripted stand-in worker.
// Built once per plugin (see the Makefile): -DTEST_<APP> selects the scenario.
#include "mock_bmw.h"
#include "drive_frame.h"
#include "gauge_core.h"
#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string>

#define ROOT ID6_PLUGIN_ROOT

namespace {
void write_file(const std::string& path, const std::string& body, bool executable) {
    FILE* f = fopen(path.c_str(), "w");
    assert(f);
    fputs(body.c_str(), f);
    fclose(f);
    if (executable) chmod(path.c_str(), 0755);
}

std::string read_file(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char buffer[512];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) out.append(buffer, n);
    fclose(f);
    return out;
}

// A stand-in worker: records its arguments, then runs `body` in /bin/sh.
std::string install(const char* app, const char* worker, const std::string& body) {
    std::string dir = std::string(ROOT) + "/" + app;
    std::string command = "mkdir -p " + dir;
    assert(system(command.c_str()) == 0);
    write_file(dir + "/gateway-ip.txt", "169.254.10.10\n", false);
    std::string log = dir + "/calls.log";
    remove(log.c_str());
    write_file(dir + "/" + worker,
               "#!/bin/sh\necho \"$$ $*\" >> " + log + "\n" + body + "\n", true);
    return log;
}

__attribute__((unused)) int count_lines(const std::string& s, const char* needle) {
    int n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}

__attribute__((unused)) bool alive(const std::string& log) {
    // The first word of the latest call is the worker's process ID.
    size_t start = log.rfind('\n', log.size() - 2);
    start = start == std::string::npos ? 0 : start + 1;
    return kill(atoi(log.c_str() + start), 0) == 0;
}
}

#ifdef TEST_WARMUP
int main() {
    std::string log = install("WarmUp", "warm-up-qnx",
        "echo 'T50 Engine oil   180'; echo 'I60 56120'; echo 'T99 outside'; echo 'junk'; "
        "printf 'T51 partial'; sleep 0.2; echo ' line'; exec sleep 30");
    bmw::PluginApp* app = mock::create("WarmUp");
    assert(mock::text[52] == "Connecting to the engine...");
    mock::visible(100, true);
    assert(mock::wait_text(app, 51, "partial line", 3000));
    assert(mock::text[50] == "Engine oil   180");
    assert(mock::image[60] == 56120);
    assert(!mock::text.count(99));
    std::string calls = read_file(log);
    assert(calls.find("--host 169.254.10.10") != std::string::npos);
    assert(alive(calls));
    mock::visible(100, false);
    mock::pump(app, 300);
    assert(!alive(calls));
    // A worker that exits on its own is restarted while the page is shown.
    log = install("WarmUp", "warm-up-qnx", "echo 'T52 run'");
    mock::visible(100, true);
    mock::pump(app, 2800);
    assert(count_lines(read_file(log), "--host") >= 2);
    mock::visible(100, false);
    mock::pump(app, 2500);
    int stopped = count_lines(read_file(log), "--host");
    mock::pump(app, 2500);
    assert(count_lines(read_file(log), "--host") == stopped);
    app->onFinalize();
    delete app;
    puts("WarmUp plugin: worker lines, model range, restart and stop passed.");
    return 0;
}
#endif

#ifdef TEST_PERFTIMER
int main() {
    std::string log = install("PerfTimer", "perf-timer-qnx", "echo 'T51 0-60 mph'; exec sleep 30");
    bmw::PluginApp* app = mock::create("PerfTimer");
    mock::visible(100, true);
    assert(mock::wait_text(app, 51, "0-60", 3000));
    mock::press(41);  // Reset best results
    mock::pump(app, 300);
    std::string calls = read_file(log);
    assert(count_lines(calls, "--state " ROOT "/PerfTimer/perf-state.txt") == 2);
    assert(count_lines(calls, "--reset") == 1);
    // Reset while hidden waits for the page to come back, and happens once.
    mock::visible(100, false);
    mock::press(41);
    mock::pump(app, 200);
    assert(count_lines(read_file(log), "--reset") == 1);
    mock::visible(100, true);
    mock::pump(app, 200);
    assert(count_lines(read_file(log), "--reset") == 2);
    mock::visible(100, false);
    mock::visible(100, true);
    mock::pump(app, 200);
    assert(count_lines(read_file(log), "--reset") == 2);
    app->onFinalize();
    delete app;
    puts("PerfTimer plugin: state file and reset handling passed.");
    return 0;
}
#endif

#ifdef TEST_FAULTSCAN
int main() {
    // Echo the requested page back the way fault-scan-qnx does.
    std::string log = install("FaultScan", "fault-scan-qnx",
        "case \"$*\" in *--show*) eval page=\\${$#}; echo \"V1 $page\"; echo \"T50 page $page\";; "
        "*--scan*) echo 'T50 scanned'; echo 'V1 0';; *--clear*) echo \"T61 cleared $6\";; esac");
    bmw::PluginApp* app = mock::create("FaultScan");
    mock::visible(100, true);
    assert(mock::wait_text(app, 50, "page 0", 3000));
    mock::press(42);
    assert(mock::wait_text(app, 50, "page 1", 3000));
    mock::press(42);
    assert(mock::wait_text(app, 50, "page 2", 3000));
    mock::press(43);
    assert(mock::wait_text(app, 50, "page 1", 3000));
    mock::press(41);
    assert(mock::wait_text(app, 50, "scanned", 3000));
    std::string calls = read_file(log);
    assert(calls.find("--file " ROOT "/FaultScan/last-scan.txt --scan") != std::string::npos);
    // Clearing needs two presses of the same button.
    mock::visible(101, true);
    mock::press(45);
    assert(mock::text[61].find("Press again") != std::string::npos);
    mock::press(44);  // a different button starts over
    assert(mock::text[61].find("engine (DME)") != std::string::npos);
    mock::pump(app, 200);
    assert(read_file(log).find("--clear") == std::string::npos);
    mock::press(44);
    assert(mock::wait_text(app, 61, "cleared 12", 3000));
    mock::press(45);
    mock::press(45);
    assert(mock::wait_text(app, 61, "cleared all", 3000));
    assert(count_lines(read_file(log), "--clear") == 2);
    app->onFinalize();
    delete app;
    puts("FaultScan plugin: paging, scan and two-press clear passed.");
    return 0;
}
#endif

#ifdef TEST_BOOSTDECK
int main() {
    std::string log = install("BoostDeck", "boostdeck-collector-qnx",
        "case \"$*\" in *--vitals*) echo 'T120 Engine oil   194'; echo 'T50 not mine';; esac; exec sleep 30");
    bmw::PluginApp* app = mock::create("BoostDeck");
    mock::visible(105, true);
    assert(mock::wait_text(app, 120, "Engine oil", 3000));
    assert(read_file(log).find("--vitals") != std::string::npos);
    assert(mock::text[50] != "not mine");  // gauge labels belong to the gauge page
    mock::visible(105, false);
    mock::pump(app, 300);
    assert(!alive(read_file(log)));
    app->onFinalize();
    delete app;
    puts("BoostDeck plugin: More vitals page passed.");
    return 0;
}
#endif

#ifdef TEST_SHIFTDECK
#include "shift_assets.h"
int main() {
    std::string log = install("ShiftDeck", "shift-deck-qnx", "exec sleep 30");
    bmw::PluginApp* app = mock::create("ShiftDeck");
    assert(mock::image[74] == kDigitBase + 10);  // "-" before any data
    mock::visible(100, true);
    mock::pump(app, 200);
    assert(read_file(log).find("--calibration " ROOT "/ShiftDeck/gear-calibration.txt") != std::string::npos);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(DRIVE_PORT);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    drive_frame frame = {bd_mono_ms(), 3000, 80, 4, 0};
    sendto(sock, &frame, sizeof frame, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof to);
    mock::pump(app, 200);
    assert(mock::image[74] == kDigitBase + 4);
    frame.sampled_ms = bd_mono_ms();
    frame.gear = 9;  // out of range: ignored
    sendto(sock, &frame, sizeof frame, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof to);
    mock::pump(app, 100);
    assert(mock::image[74] == kDigitBase + 4);
    frame.sampled_ms = bd_mono_ms();
    frame.gear = 0;
    sendto(sock, &frame, sizeof frame, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof to);
    mock::pump(app, 100);
    assert(mock::image[74] == kDigitBase + 10);
    close(sock);
    mock::visible(100, false);
    app->onFinalize();
    delete app;
    puts("ShiftDeck plugin: gear indicator passed.");
    return 0;
}
#endif
