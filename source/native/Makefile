CC ?= cc
CXX ?= c++
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c
CORE_LINK = $(CORE) src/diag_link.c
HEADERS = src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h src/diag_link.h
# Every host worker carries the HSFZ-then-MOST link, with a fake DSI selected by ID6_FAKE_MOST.
MOST = src/most/most_link.c src/most/dsi_client.c src/most/most_proto.c src/most/dsi_bind.c
MOST_HEADERS = src/most/most_link.h src/most/dsi_client.h src/most/most_proto.h src/most/dsi_abi.h
FAKE_DSI = tests/fake_dsi.c tests/fake_dme.c tests/fake_dsi.h tests/fake_dme.h
# The text apps' workers share app_worker.c; drive_core.c holds their pure logic and ShiftDeck's gear.
APP = src/app_worker.c src/drive_core.c
APP_HEADERS = src/app_worker.h src/drive_core.h src/app_assets.h
LINK = $(CORE_LINK) $(APP) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(APP_HEADERS) $(MOST_HEADERS)
WORKER = src/collector.c src/vitals.c $(LINK)
HOST_LINK = $(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests
.PHONY: assets text-assets test

assets:
	$(PYTHON) tools/make_bars.py
	$(PYTHON) tools/make_drive_apps.py

# The three text apps' layouts and image archives (make_bars.py and make_drive_apps.py predate the
# published BoostDeck and ShiftDeck layouts, which are now edited in plugin/ directly).
text-assets:
	cd tools && $(PYTHON) make_text_apps.py

build:
	mkdir -p build

build/boostdeck-collector: $(WORKER) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/shift-deck: $(WORKER) | build
	$(HOST_LINK) -DSHIFT_DECK_ONLY $(filter %.c,$^) -lm -o $@

build/mhd-maps: $(WORKER) | build
	$(HOST_LINK) -DMHD_MAPS_ONLY $(filter %.c,$^) -lm -o $@

build/engine-codes: $(WORKER) | build
	$(HOST_LINK) -DENGINE_CODES_ONLY $(filter %.c,$^) -lm -o $@

build/warm-up: src/warm_up.c $(LINK) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/perf-timer: src/perf_timer.c $(LINK) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/fault-scan: src/fault_scan.c $(LINK) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

build/test-drive-core: tests/test_drive_core.c src/drive_core.c src/drive_core.h | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/test-core: tests/test_core.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/test-most: tests/test_most.c $(MOST) $(FAKE_DSI) $(MOST_HEADERS) | build
	$(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests $(filter %.c,$^) -o $@

build/test-diag-link: tests/test_diag_link.c $(CORE_LINK) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

# Plugins on the host: the real plugin sources against tests/mock_bmw.cpp, with build.sh's defines.
CXXFLAGS ?= -O1 -std=gnu++98 -Wall -Wextra -Werror -fno-strict-aliasing
PLUGIN_TESTS = build/test-plugin-WARMUP build/test-plugin-PERFTIMER build/test-plugin-FAULTSCAN \
	build/test-plugin-BOOSTDECK build/test-plugin-SHIFTDECK
SOURCE_WARMUP = src/warm_up_app.cpp
SOURCE_PERFTIMER = src/perf_timer_app.cpp
SOURCE_FAULTSCAN = src/fault_scan_app.cpp
SOURCE_BOOSTDECK = src/plugin.cpp
SOURCE_SHIFTDECK = src/shift_deck.cpp
DEFINES_BOOSTDECK = -DPERF_LAB
DEFINES_SHIFTDECK = -DPERF_LAB -DNATIVE_SHIFT_SMOOTH
build/core-host.o: src/gauge_core.c src/gauge_core.h | build
	$(CC) $(CFLAGS) -DPERF_LAB -Isrc -c $< -o $@
.SECONDEXPANSION:
build/test-plugin-%: tests/test_plugins.cpp tests/mock_bmw.cpp tests/mock_bmw.h $$(SOURCE_$$*) \
		src/text_app.h src/diagnostic_gateway.h src/shift_assets.h src/drive_frame.h build/core-host.o | build
	$(CXX) $(CXXFLAGS) -Isrc -Isdk -Itests -DTEST_$* $(DEFINES_$*) '-DID6_PLUGIN_ROOT="build/plugin-root"' \
		tests/test_plugins.cpp tests/mock_bmw.cpp $(SOURCE_$*) build/core-host.o -lm -o $@

WORKERS = build/boostdeck-collector build/shift-deck build/mhd-maps build/engine-codes build/warm-up \
	build/perf-timer build/fault-scan

test: text-assets $(WORKERS) build/test-core build/test-drive-core build/test-most build/test-diag-link \
		$(PLUGIN_TESTS)
	./build/test-core
	./build/test-drive-core
	for t in $(PLUGIN_TESTS); do ./$$t || exit 1; done
	./build/test-most
	./build/test-diag-link
	$(PYTHON) -B tests/test_drive_apps.py
	$(PYTHON) -B tests/test_oneshot_most.py
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_text_apps.py
	$(PYTHON) -B tests/test_text_layouts.py
	$(PYTHON) -B tests/test_stage_apps.py
	$(PYTHON) -B tests/test_rescale_layout.py
