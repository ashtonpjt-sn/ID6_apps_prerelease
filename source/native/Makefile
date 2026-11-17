CC ?= cc
PYTHON ?= python3
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CORE = src/gauge_core.c
CORE_LINK = $(CORE) src/diag_link.c
HEADERS = src/gauge_core.h src/drive_frame.h src/diagnostic_lease.h src/diag_link.h
# Every host worker carries the HSFZ-then-MOST link, with a fake DSI selected by ID6_FAKE_MOST.
MOST = src/most/most_link.c src/most/dsi_client.c src/most/most_proto.c src/most/dsi_bind.c
MOST_HEADERS = src/most/most_link.h src/most/dsi_client.h src/most/most_proto.h src/most/dsi_abi.h
FAKE_DSI = tests/fake_dsi.c tests/fake_dme.c tests/fake_dsi.h tests/fake_dme.h
WORKER = src/collector.c $(CORE_LINK) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS)
HOST_LINK = $(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests
.PHONY: assets test

assets:
	$(PYTHON) tools/make_bars.py
	$(PYTHON) tools/make_drive_apps.py

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

build/test-core: tests/test_core.c $(CORE) $(HEADERS) | build
	$(CC) $(CFLAGS) -Isrc $(filter %.c,$^) -lm -o $@

build/test-most: tests/test_most.c $(MOST) $(FAKE_DSI) $(MOST_HEADERS) | build
	$(CC) $(CFLAGS) -pthread -DMOST_HOST -Isrc -Itests $(filter %.c,$^) -o $@

build/test-diag-link: tests/test_diag_link.c $(CORE_LINK) $(MOST) tests/fake_most_env.c $(FAKE_DSI) $(HEADERS) $(MOST_HEADERS) | build
	$(HOST_LINK) $(filter %.c,$^) -lm -o $@

test: build/boostdeck-collector build/shift-deck build/mhd-maps build/engine-codes build/test-core build/test-most build/test-diag-link
	./build/test-core
	./build/test-most
	./build/test-diag-link
	$(PYTHON) -B tests/test_drive_apps.py
	$(PYTHON) -B tests/test_oneshot_most.py
	$(PYTHON) -B tests/test_transport.py
	$(PYTHON) -B tests/test_rescale_layout.py
