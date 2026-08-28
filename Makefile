CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
GBDK_HOME ?= /opt/gbdk
LCC ?= $(GBDK_HOME)/bin/lcc
GEARSYSTEM_DIR ?=

VERSION := $(shell cat VERSION)
PROJECT := FEAR-Z80-Tactician
ROM_BASENAME := $(PROJECT)-v$(VERSION)

HOST_BIN := build/host_preview
TEST_BIN := build/test_sim
GG_ROM := build/$(ROM_BASENAME)-seed2.gg
GG_SEED42_ROM := build/$(ROM_BASENAME)-seed42.gg
ROM_DIR := roms
RELEASE_ROM := $(ROM_DIR)/$(ROM_BASENAME)-seed2.gg
RELEASE_SEED42_ROM := $(ROM_DIR)/$(ROM_BASENAME)-seed42.gg
GG_SRC_FIXED := src/main_gg.c src/sim.c src/tiles.c
GG_SRC_BANKED := src/brain.c
GG_OBJS := build/main_gg.o build/sim.o build/tiles.o build/brain.o
SMOKE_ROM := build/gg_smoke.gg

TILESECTOR_TEST_BIN := build/test_tilesector
TILESECTOR_HOST_BIN := build/tilesector_preview
TILESECTOR_ROM := build/gg-tilesector-demo.gg
TILESECTOR_GG_OBJS := build/main_tilesector_gg.o build/tilesector_core_gg.o build/tilesector_vram_gg.o build/tilesector_raster_gg.o

POLAR_TEST_BIN := build/test_tilesector_polar
POLAR_ROM := build/gg-tilesector-polar.gg
POLAR_GG_OBJS := build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o build/tilesector_polar_renderer_gg.o build/tilesector_polar_frame_gg.o build/tilesector_vram_gg.o

GGFLAGS := -mz80:gg -debug -autobank -Wb-ext=.rel -Wl-j -Wm-yo4 -Isrc
# Speed bias is cheap enough for normal iteration. GBDK's documented
# --max-allocs-per-node50000 experiment is measured separately because the
# inlined renderer makes compile time explode.
TILESECTOR_FASTFLAGS := -Wf--opt-code-speed

.PHONY: all host test gg gg-seed42 release smoke gear-tools emu-smoke tilesector-test tilesector-host gg-tilesector polar-test gg-tilesector-polar clean
all: host test

build:
	mkdir -p build

host: build
	$(CC) $(CFLAGS) -Isrc src/sim.c src/brain.c host/main_host.c -o $(HOST_BIN)

test: build
	$(CC) $(CFLAGS) -Isrc src/sim.c src/brain.c tests/test_sim.c -o $(TEST_BIN)
	./$(TEST_BIN)

build/main_gg.o: src/main_gg.c | build
	$(LCC) $(GGFLAGS) -DDEFAULT_SEED=2u -c -o $@ $<
build/sim.o: src/sim.c | build
	$(LCC) $(GGFLAGS) -c -o $@ $<
build/tiles.o: src/tiles.c | build
	$(LCC) $(GGFLAGS) -c -o $@ $<
build/brain.o: src/brain.c | build
	$(LCC) $(GGFLAGS) -c -o $@ $<

gg: $(GG_OBJS)
	$(LCC) $(GGFLAGS) -o $(GG_ROM) $(GG_OBJS)

build/main_gg_seed42.o: src/main_gg.c | build
	$(LCC) $(GGFLAGS) -DDEFAULT_SEED=42u -c -o $@ $<

gg-seed42: build/main_gg_seed42.o build/sim.o build/tiles.o build/brain.o
	$(LCC) $(GGFLAGS) -o $(GG_SEED42_ROM) build/main_gg_seed42.o build/sim.o build/tiles.o build/brain.o

release: gg gg-seed42
	mkdir -p $(ROM_DIR)
	cp $(GG_ROM) $(RELEASE_ROM)
	cp $(GG_SEED42_ROM) $(RELEASE_SEED42_ROM)
	sha256sum $(RELEASE_ROM) $(RELEASE_SEED42_ROM) > $(ROM_DIR)/SHA256SUMS-v$(VERSION).txt

smoke: build
	python3 tools/make_smoke_rom.py $(SMOKE_ROM)

gear-tools: build
	@test -n "$(GEARSYSTEM_DIR)" || (echo "Set GEARSYSTEM_DIR=/path/to/Gearsystem-3.9.16"; exit 2)
	tools/build_gearsystem_tools.sh "$(GEARSYSTEM_DIR)"

emu-smoke: smoke gear-tools
	./build/libretro_runner "$(GEARSYSTEM_DIR)/platforms/libretro/gearsystem_libretro.so" $(SMOKE_ROM) 30 build/gg_smoke.ppm
	./build/gearsystem_core_runner $(SMOKE_ROM) 30

tilesector-test: build
	$(CC) $(CFLAGS) -Isrc src/tilesector_core.c tests/test_tilesector.c -o $(TILESECTOR_TEST_BIN)
	./$(TILESECTOR_TEST_BIN)

tilesector-host: build
	$(CC) $(CFLAGS) -Isrc src/tilesector_core.c host/tilesector_host.c -o $(TILESECTOR_HOST_BIN)

polar-test: build
	$(CC) $(CFLAGS) -Isrc src/tilesector_polar_motion.c src/tilesector_polar_renderer.c tests/test_tilesector_polar.c -o $(POLAR_TEST_BIN)
	./$(POLAR_TEST_BIN)

build/main_tilesector_gg.o: src/main_tilesector_gg.c | build
	$(LCC) $(GGFLAGS) $(TILESECTOR_FASTFLAGS) -c -o $@ $<

build/tilesector_core_gg.o: src/tilesector_core.c | build
	$(LCC) $(GGFLAGS) $(TILESECTOR_FASTFLAGS) -c -o $@ $<

build/tilesector_vram_gg.o: src/tilesector_vram_gg.s | build
	$(LCC) $(GGFLAGS) -c -o $@ $<

build/tilesector_raster_gg.o: src/tilesector_raster_gg.s | build
	$(LCC) $(GGFLAGS) -c -o $@ $<

gg-tilesector: $(TILESECTOR_GG_OBJS)
	$(LCC) $(GGFLAGS) -Wm-yS -o $(TILESECTOR_ROM) $(TILESECTOR_GG_OBJS)

build/main_tilesector_polar_gg.o: src/main_tilesector_polar_gg.c | build
	$(LCC) $(GGFLAGS) $(TILESECTOR_FASTFLAGS) -c -o $@ $<

build/tilesector_polar_motion_gg.o: src/tilesector_polar_motion.c | build
	$(LCC) $(GGFLAGS) $(TILESECTOR_FASTFLAGS) -c -o $@ $<

build/tilesector_polar_renderer_gg.o: src/tilesector_polar_renderer.c | build
	$(LCC) $(GGFLAGS) $(TILESECTOR_FASTFLAGS) -c -o $@ $<

gg-tilesector-polar: $(POLAR_GG_OBJS)
	$(LCC) $(GGFLAGS) -Wm-yS -o $(POLAR_ROM) $(POLAR_GG_OBJS)

clean:
	rm -rf build/*
