CC ?= cc
CXX ?= c++
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c src/hsfz.c src/drive_core.c
HEADERS = src/hsfz.h src/drive_core.h src/app_assets.h src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h
.PHONY: assets test

assets:
	$(PYTHON) tools/make_bars.py
	$(PYTHON) tools/make_drive_apps.py
	cd tools && $(PYTHON) make_text_apps.py

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

# Plugins on the host: the real plugin sources against tests/mock_bmw.cpp.
CXXFLAGS ?= -O1 -std=gnu++98 -Wall -Wextra -Werror -fno-strict-aliasing
PLUGIN_TESTS = build/test-plugin-WARMUP build/test-plugin-PERFTIMER build/test-plugin-FAULTSCAN \
	build/test-plugin-BOOSTDECK build/test-plugin-SHIFTDECK
SOURCE_WARMUP = src/warm_up_app.cpp
SOURCE_PERFTIMER = src/perf_timer_app.cpp
SOURCE_FAULTSCAN = src/fault_scan_app.cpp
SOURCE_BOOSTDECK = src/plugin.cpp
SOURCE_SHIFTDECK = src/shift_deck.cpp
build/core-host.o: src/gauge_core.c src/gauge_core.h | build
	$(CC) $(CFLAGS) -Isrc -c $< -o $@
.SECONDEXPANSION:
build/test-plugin-%: tests/test_plugins.cpp tests/mock_bmw.cpp tests/mock_bmw.h $$(SOURCE_$$*) \
		src/text_app.h build/core-host.o | build
	$(CXX) $(CXXFLAGS) -Isrc -Isdk -Itests -DTEST_$* '-DID6_PLUGIN_ROOT="build/plugin-root"' \
		tests/test_plugins.cpp tests/mock_bmw.cpp $(SOURCE_$*) build/core-host.o -lm -o $@

WORKERS = build/boostdeck-collector build/shift-deck build/mhd-maps build/warm-up \
	build/perf-timer build/fault-scan

test: $(WORKERS) build/test-core build/test-drive-core $(PLUGIN_TESTS)
	./build/test-core
	./build/test-drive-core
	for t in $(PLUGIN_TESTS); do ./$$t || exit 1; done
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_drive_apps.py
	$(PYTHON) -B tests/test_text_apps.py
