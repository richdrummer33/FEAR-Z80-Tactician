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
POLAR_TRANSITION_BAKE_BIN := build/polar_transition_bake
POLAR_DEMO_PATCH_GEN_BIN := build/polar_demo_patch_gen
POLAR_PATCH_ROM := build/gg-polar-patch-demo.gg
POLAR_PATCH_GEN_DIR := build/generated/polar_demo_patch
POLAR_PATCH_STAMP := $(POLAR_PATCH_GEN_DIR)/.stamp
POLAR_PATCH_META := $(POLAR_PATCH_GEN_DIR)/polar_demo_patch_meta.h
POLAR_PATCH_BANKS := 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
POLAR_PATCH_SRCS := $(addprefix $(POLAR_PATCH_GEN_DIR)/polar_demo_patch_bank,$(addsuffix .c,$(POLAR_PATCH_BANKS)))
POLAR_PATCH_OBJS := $(addprefix build/polar_demo_patch_bank,$(addsuffix _gg.o,$(POLAR_PATCH_BANKS)))
POLAR_PATCH_DISPATCH_SRC := $(POLAR_PATCH_GEN_DIR)/polar_demo_patch_dispatch.c
POLAR_PATCH_DISPATCH_OBJ := build/polar_demo_patch_dispatch_gg.o
POLAR_PATCH_GGFLAGS := $(filter-out -Wm-yo4,$(GGFLAGS)) -Wm-yo32 -I$(POLAR_PATCH_GEN_DIR)
POLAR_PATCH_GG_OBJS := build/main_tilesector_polar_patch_gg.o build/tilesector_polar_patch_motion_gg.o build/tilesector_polar_patch_ntstate_gg.o build/tilesector_polar_patch_ntupload_raw_gg.o $(POLAR_PATCH_DISPATCH_OBJ) $(POLAR_PATCH_OBJS)
POLAR_ROM := build/gg-tilesector-polar.gg
POLAR_PROFILE_HOOKS ?= 1
POLAR_LOCAL_PROJECTION ?= 1
POLAR_SCREEN_DEPTH_PLANE ?= 1
POLAR_PROJ_THRESHOLD ?= 4
POLAR_PROJ_ROWS_PER_BANK ?= 4
POLAR_PROJ_BANK_COUNT ?= 6
POLAR_ROM_BANKS ?= 8
POLAR_PROJ_GEN_DIR := build/generated/polar_projection
POLAR_PROJ_STAMP := $(POLAR_PROJ_GEN_DIR)/.stamp
POLAR_PROJ_META := $(POLAR_PROJ_GEN_DIR)/tilesector_polar_projection_meta.h
POLAR_DEPTHPLANE_GEN_DIR := build/generated/polar_depthplane
POLAR_DEPTHPLANE_HDR := $(POLAR_DEPTHPLANE_GEN_DIR)/tilesector_polar_depthplane_lut.h
POLAR_DEPTHPLANE_SRC := $(POLAR_DEPTHPLANE_GEN_DIR)/tilesector_polar_depthplane.c
POLAR_DEPTHPLANE_OBJ := build/tilesector_polar_depthplane_gg.o
POLAR_PROJ_BANKS_6 := 0 1 2 3 4 5
POLAR_PROJ_BANKS_24 := 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23
POLAR_PROJ_BANKS := $(POLAR_PROJ_BANKS_$(POLAR_PROJ_BANK_COUNT))
POLAR_PROJ_SRCS := $(addprefix $(POLAR_PROJ_GEN_DIR)/tilesector_polar_proj_bank,$(addsuffix .c,$(POLAR_PROJ_BANKS)))
POLAR_PROJ_OBJS := $(addprefix build/tilesector_polar_proj_bank,$(addsuffix .o,$(POLAR_PROJ_BANKS)))
POLAR_GGFLAGS = $(filter-out -Wm-yo4,$(GGFLAGS)) -Wm-yo$(POLAR_ROM_BANKS) -I$(POLAR_PROJ_GEN_DIR) -I$(POLAR_DEPTHPLANE_GEN_DIR)
POLAR_CFLAGS = $(TILESECTOR_FASTFLAGS) -DTSPF_PROFILE_HOOKS=$(POLAR_PROFILE_HOOKS) -DTSPF_LOCAL_PROJECTION=$(POLAR_LOCAL_PROJECTION) -DTSPF_SCREEN_DEPTH_PLANE=$(POLAR_SCREEN_DEPTH_PLANE)
ifeq ($(POLAR_PROFILE_HOOKS),0)
POLAR_NTUPLOAD_OBJ := build/tilesector_polar_ntupload_raw_gg.o
else
POLAR_NTUPLOAD_OBJ := build/tilesector_polar_ntupload_profiled_gg.o
endif
POLAR_GG_OBJS := build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o build/tilesector_polar_renderer_gg.o build/tilesector_polar_ntstate_gg.o build/tilesector_polar_materialize_gg.o $(POLAR_NTUPLOAD_OBJ)
ifeq ($(POLAR_LOCAL_PROJECTION),1)
POLAR_GG_OBJS += build/tilesector_polar_projection_gg.o $(POLAR_PROJ_OBJS)
endif
ifeq ($(POLAR_SCREEN_DEPTH_PLANE),1)
POLAR_GG_OBJS += $(POLAR_DEPTHPLANE_OBJ)
endif

GGFLAGS := -mz80:gg -debug -autobank -Wb-ext=.rel -Wl-j -Wm-yo4 -Isrc
# Speed bias is cheap enough for normal iteration. GBDK's documented
# --max-allocs-per-node50000 experiment is measured separately because the
# inlined renderer makes compile time explode.
TILESECTOR_FASTFLAGS := -Wf--opt-code-speed

.PHONY: all host test gg gg-seed42 release smoke gear-tools emu-smoke tilesector-test tilesector-host gg-tilesector polar-test polar-transition-bake polar-demo-patch-gen gg-polar-patch-demo gg-tilesector-polar clean
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

$(POLAR_TRANSITION_BAKE_BIN): src/tilesector_polar_motion.c src/tilesector_polar_renderer.c tools/polar_transition_bake.c | build
	$(CC) $(CFLAGS) -Isrc src/tilesector_polar_motion.c src/tilesector_polar_renderer.c tools/polar_transition_bake.c -o $@

polar-transition-bake: $(POLAR_TRANSITION_BAKE_BIN)

$(POLAR_DEMO_PATCH_GEN_BIN): src/tilesector_polar_motion.c src/tilesector_polar_renderer.c tools/polar_demo_patch_gen.c | build
	$(CC) $(CFLAGS) -Isrc src/tilesector_polar_motion.c src/tilesector_polar_renderer.c tools/polar_demo_patch_gen.c -o $@

$(POLAR_PATCH_STAMP): $(POLAR_DEMO_PATCH_GEN_BIN) | build
	mkdir -p $(POLAR_PATCH_GEN_DIR)
	$(POLAR_DEMO_PATCH_GEN_BIN) $(POLAR_PATCH_GEN_DIR)
	touch $@

$(POLAR_PATCH_META) $(POLAR_PATCH_SRCS) $(POLAR_PATCH_DISPATCH_SRC): $(POLAR_PATCH_STAMP)
	@:

polar-demo-patch-gen: $(POLAR_PATCH_STAMP)

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

$(POLAR_PROJ_STAMP): experiments/adaptive_polar_field/local_projection_field_poc.py src/generated/tilesector_polar_data_part00.inc src/generated/tilesector_polar_data_part01.inc src/generated/tilesector_polar_data_part02.inc src/generated/tilesector_polar_data_part03.inc src/generated/tilesector_polar_data_part04.inc | build
	mkdir -p $(POLAR_PROJ_GEN_DIR)
	python3 experiments/adaptive_polar_field/local_projection_field_poc.py --emit-dir $(POLAR_PROJ_GEN_DIR) --emit-threshold $(POLAR_PROJ_THRESHOLD) --min-q4 8 --rows-per-bank $(POLAR_PROJ_ROWS_PER_BANK) --emit-only
	touch $@

$(POLAR_PROJ_META) $(POLAR_PROJ_SRCS): $(POLAR_PROJ_STAMP)
	@:

$(POLAR_DEPTHPLANE_HDR) $(POLAR_DEPTHPLANE_SRC): experiments/adaptive_polar_field/screen_depth_plane_lut.py src/generated/tilesector_polar_data_part00.inc src/generated/tilesector_polar_data_part01.inc src/generated/tilesector_polar_data_part02.inc src/generated/tilesector_polar_data_part03.inc src/generated/tilesector_polar_data_part04.inc | build
	mkdir -p $(POLAR_DEPTHPLANE_GEN_DIR)
	python3 experiments/adaptive_polar_field/screen_depth_plane_lut.py --emit-dir $(POLAR_DEPTHPLANE_GEN_DIR)

$(POLAR_DEPTHPLANE_OBJ): $(POLAR_DEPTHPLANE_SRC) $(POLAR_DEPTHPLANE_HDR) | build
	$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ $(POLAR_DEPTHPLANE_SRC)

build/tilesector_polar_proj_bank%.o: $(POLAR_PROJ_GEN_DIR)/tilesector_polar_proj_bank%.c $(POLAR_PROJ_META) | build
	$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ $<

build/main_tilesector_polar_gg.o: src/main_tilesector_polar_gg.c | build
	$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ $<

build/tilesector_polar_motion_gg.o: src/tilesector_polar_motion.c | build
	$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ $<

build/tilesector_polar_renderer_gg.o: src/tilesector_polar_renderer.c $(POLAR_PROJ_META) $(POLAR_DEPTHPLANE_HDR) | build
	$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ $<

build/tilesector_polar_frame_gg.o: src/tilesector_polar_frame_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_materialize_gg.o: src/tilesector_polar_materialize_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_projection_gg.o: src/tilesector_polar_projection_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_ntstate_gg.o: src/tilesector_polar_ntstate_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_ntupload_profiled_gg.o: src/tilesector_polar_ntupload_profiled_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_ntupload_raw_gg.o: src/tilesector_polar_ntupload_raw_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/main_tilesector_polar_patch_gg.o: src/main_tilesector_polar_patch_gg.c $(POLAR_PATCH_META) | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -DTSPF_PROFILE_HOOKS=0 -c -o $@ $<

build/tilesector_polar_patch_motion_gg.o: src/tilesector_polar_motion.c | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -DTSPF_PROFILE_HOOKS=0 -c -o $@ $<

build/tilesector_polar_patch_ntstate_gg.o: src/tilesector_polar_ntstate_gg.s | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -c -o $@ $<

build/tilesector_polar_patch_ntupload_raw_gg.o: src/tilesector_polar_ntupload_raw_gg.s | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -c -o $@ $<

build/polar_demo_patch_dispatch_gg.o: $(POLAR_PATCH_DISPATCH_SRC) $(POLAR_PATCH_META) | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -DTSPF_PROFILE_HOOKS=0 -c -o $@ $<

build/polar_demo_patch_bank%_gg.o: $(POLAR_PATCH_GEN_DIR)/polar_demo_patch_bank%.c $(POLAR_PATCH_META) | build
	$(LCC) $(POLAR_PATCH_GGFLAGS) -DTSPF_PROFILE_HOOKS=0 -c -o $@ $<

gg-polar-patch-demo: $(POLAR_PATCH_GG_OBJS)
	$(LCC) $(POLAR_PATCH_GGFLAGS) -Wm-yS -o $(POLAR_PATCH_ROM) $(POLAR_PATCH_GG_OBJS)

build/tilesector_polar_vram_profiled_gg.o: src/tilesector_polar_vram_profiled_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

build/tilesector_polar_vram_raw_gg.o: src/tilesector_polar_vram_raw_gg.s | build
	$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

gg-tilesector-polar: $(POLAR_GG_OBJS)
	$(LCC) $(POLAR_GGFLAGS) -Wm-yS -o $(POLAR_ROM) $(POLAR_GG_OBJS)

clean:
	rm -rf build/*
