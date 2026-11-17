#!/bin/sh
set -eu
cd "$(dirname "$0")"
export QNX_HOST="${QNX_HOST:-/opt/qnx650/host/linux/x86}"
export QNX_TARGET="${QNX_TARGET:-/opt/qnx650/target/qnx6}"
export QNX_CONFIGURATION="${QNX_CONFIGURATION:-/etc/qnx}"
frameworks="${NBTEVO_FRAMEWORKS_DIR:-$PWD/frameworks}"
runtime="${NBTEVO_RUNTIME_DIR:-$PWD/runtime}"
for lib in libAppFramework.A.so libCoreFramework.A.so; do
    test -f "$frameworks/$lib" || { echo "Missing private framework: $frameworks/$lib" >&2; exit 1; }
done
export PATH="$QNX_HOST/usr/bin:$PATH"
mkdir -p out
for pair in libm.so:libm.so.2 libsocket.so:libsocket.so.3 libstdc++.so:libstdc++.so.6; do
    name=${pair%:*}
    versioned=${pair#*:}
    if test -f "$runtime/$versioned"; then ln -sf "$runtime/$versioned" "out/$name"; fi
done
ntoarm-gcc -dumpversion
ntoarm-gcc -dumpmachine
c() { qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=c99 -Wall -Wextra -Werror "$@"; }
cxx() { qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ -Wc,-std=gnu++98 -Wall -Wextra -Werror "$@"; }
c -DPERF_LAB -Isrc -O2 -fPIC -c src/gauge_core.c -o out/core.o
cxx -DPERF_LAB -Isrc -Isdk -O2 -fno-strict-aliasing -fno-exceptions -fPIC -shared src/plugin.cpp out/core.o \
    "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lm -lsocket \
    -Wl,--allow-shlib-undefined -Wl,-soname,libBoostDeck.so -o out/libBoostDeck.so
# Every worker carries the HSFZ-then-MOST link. dlopen and pthreads are in QNX libc; the DSI library is
# opened only at run time, so nothing private is linked.
worker() { c -Isrc -O2 "$@" src/diag_link.c src/most/*.c -Lout -lm -lsocket; }
# The collector also serves BoostDeck's More vitals page (--vitals, vitals.c).
worker -DNATIVE_FAST -DPERF_LAB src/collector.c src/vitals.c src/app_worker.c out/core.o -o out/boostdeck-collector-qnx
cxx -DPERF_LAB -Isrc -Isdk -O2 -fno-exceptions -fPIC -shared src/mhd_maps.cpp \
    "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout \
    -Wl,--allow-shlib-undefined -Wl,-soname,libMhdMaps.so -o out/libMhdMaps.so
cxx -DPERF_LAB -Isrc -Isdk -O2 -fno-exceptions -fPIC -shared -DNATIVE_SHIFT_SMOOTH src/shift_deck.cpp out/core.o \
    "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lm -lsocket \
    -Wl,--allow-shlib-undefined -Wl,-soname,libShiftDeck.so -o out/libShiftDeck.so
worker -DPERF_LAB -DMHD_MAPS_ONLY src/collector.c src/gauge_core.c -o out/mhd-maps-qnx
worker -DNATIVE_FAST -DSHIFT_DECK_ONLY src/collector.c src/drive_core.c out/core.o -o out/shift-deck-qnx
# Warm-Up, Performance Timer and Fault Scanner: a worker each and a TextApp plugin (text_app.h).
for app in WarmUp PerfTimer FaultScan; do
    case "$app" in
        WarmUp) source=warm_up; worker=warm-up-qnx ;;
        PerfTimer) source=perf_timer; worker=perf-timer-qnx ;;
        FaultScan) source=fault_scan; worker=fault-scan-qnx ;;
    esac
    worker "src/$source.c" src/app_worker.c src/drive_core.c out/core.o -o "out/$worker"
    cxx -Isrc -Isdk -O2 -fno-strict-aliasing -fno-exceptions -fPIC -shared "src/${source}_app.cpp" out/core.o \
        "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lm -lsocket \
        -Wl,--allow-shlib-undefined "-Wl,-soname,lib$app.so" -o "out/lib$app.so"
done
c -O2 -static fileops.c -o out/gaugedeck-fileops
c -O2 src/exhaust_flap_launcher.c -lsocket -o out/exhaust-flap-qnx
cxx -DPERF_LAB -Isrc -Isdk -O2 -fno-strict-aliasing -fno-exceptions -fPIC -shared src/engine_codes.cpp out/core.o \
    "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lm -lsocket \
    -Wl,--allow-shlib-undefined -Wl,-soname,libEngineCodes.so -o out/libEngineCodes.so
worker -DENGINE_CODES_ONLY src/collector.c out/core.o -o out/engine-codes-qnx
for app in Bootmod3Suite XDriveControl; do
    case "$app" in
        Bootmod3Suite) flag=-DBM3_ONLY; worker=bootmod3-qnx ;;
        XDriveControl) flag=-DXDRIVE_ONLY; worker=xdrive-control-qnx ;;
    esac
    worker "$flag" src/collector.c out/core.o -o "out/$worker"
    cxx -Isrc -Isdk -O2 "$flag" -fno-exceptions -fPIC -shared src/mhd_maps.cpp \
        "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lsocket \
        -Wl,--allow-shlib-undefined "-Wl,-soname,lib$app.so" -o "out/lib$app.so"
done
