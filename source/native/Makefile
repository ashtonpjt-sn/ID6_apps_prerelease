CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c src/hsfz.c src/drive_core.c
HEADERS = src/hsfz.h src/drive_core.h src/app_assets.h src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h
.PHONY: assets test

assets:
	$(PYTHON) tools/make_bars.py
	$(PYTHON) tools/make_drive_apps.py

build:
	mkdir -p build

build/boostdeck-collector: src/collector.c src/vitals.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/shift-deck: src/collector.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -DSHIFT_DECK_ONLY $(filter %.c,$^) -lm -o $@

build/mhd-maps: src/collector.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc -DMHD_MAPS_ONLY $(filter %.c,$^) -lm -o $@

build/test-core: tests/test_core.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/warm-up: src/warm_up.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/perf-timer: src/perf_timer.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/fault-scan: src/fault_scan.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/test-drive-core: tests/test_drive_core.c src/drive_core.c src/drive_core.h | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

WORKERS = build/boostdeck-collector build/shift-deck build/mhd-maps build/warm-up \
	build/perf-timer build/fault-scan

test: $(WORKERS) build/test-core build/test-drive-core
	./build/test-core
	./build/test-drive-core
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_drive_apps.py
