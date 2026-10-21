# Scorched Earth eMiYoolator
#
#   make                 desktop build (SDL2) with the recompiled game: build/host/scorch
#   make interp          desktop build, interpreter only: build/host/scorch-interp
#   make miyoo           ARM build for the Miyoo Mini (run inside the toolchain
#                        container, or use scripts/build-miyoo.sh): build/miyoo/scorch
#   make recompile       regenerate build/gen/*.c from $(EXE)
#
# The game's code is translated from the user's own SCORCH.EXE at build time;
# nothing derived from it is part of the sources.

EXE ?= Original_EXE/SCORCH.EXE

RUNTIME := machine interp decode ops fpu dos bios vga pit kbd mouse ui script main
GEN_FONT := build/gen/vgafont.c

CFLAGS_COMMON := -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
	-D_FILE_OFFSET_BITS=64 -Isrc

HOST_CC ?= cc
HOST_CFLAGS = $(CFLAGS_COMMON) $(shell sdl2-config --cflags)
HOST_LIBS = $(shell sdl2-config --libs) -lm -lpthread

MIYOO_CC ?= arm-linux-gnueabihf-gcc
MIYOO_CFLAGS := $(CFLAGS_COMMON) -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
	-DPLATFORM_MIYOO
MIYOO_LIBS := -lm -ldl -lpthread

HOST_RT := $(patsubst %,build/host/%.o,$(RUNTIME)) build/host/vgafont.o build/host/platform_sdl.o
MIYOO_RT := $(patsubst %,build/miyoo/%.o,$(RUNTIME)) build/miyoo/vgafont.o build/miyoo/platform_miyoo.o

# generated game code (one file per x86 code segment) and its index
# (plain C from tools/recomp.c; -g0 keeps the build quick)
GEN_OPT := -O2 -g0
GEN_SRC = $(sort $(wildcard build/gen/seg_*.c)) build/gen/index.c
HOST_GEN = $(patsubst build/gen/%.c,build/host/gen/%.o,$(GEN_SRC))
MIYOO_GEN = $(patsubst build/gen/%.c,build/miyoo/gen/%.o,$(GEN_SRC))

.PHONY: all host interp miyoo recompile gen clean tools

all: host

host: build/host/scorch

interp: build/host/scorch-interp

miyoo: build/miyoo/scorch

tools: build/tools/recomp

build/host/scorch-interp: $(HOST_RT) build/host/compiled_none.o
	$(HOST_CC) -o $@ $^ $(HOST_LIBS)

build/host/scorch: $(HOST_RT) $(HOST_GEN)
	$(HOST_CC) -o $@ $(HOST_RT) $(HOST_GEN) $(HOST_LIBS)

build/miyoo/scorch: $(MIYOO_RT) $(MIYOO_GEN)
	$(MIYOO_CC) -o $@ $(MIYOO_RT) $(MIYOO_GEN) $(MIYOO_LIBS)

build/host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -MMD -c -o $@ $<

build/miyoo/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(MIYOO_CC) $(MIYOO_CFLAGS) -MMD -c -o $@ $<

build/host/vgafont.o: $(GEN_FONT)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

build/miyoo/vgafont.o: $(GEN_FONT)
	@mkdir -p $(dir $@)
	$(MIYOO_CC) $(MIYOO_CFLAGS) -c -o $@ $<

build/host/gen/%.o: build/gen/%.c src/cpu.h src/ops.h src/insn.h src/gen.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(GEN_OPT) -c -o $@ $<

build/miyoo/gen/%.o: build/gen/%.c src/cpu.h src/ops.h src/insn.h src/gen.h
	@mkdir -p $(dir $@)
	$(MIYOO_CC) $(MIYOO_CFLAGS) $(GEN_OPT) -c -o $@ $<

$(GEN_FONT): build/fonts.bin scripts/bin2c.py
	@mkdir -p $(dir $@)
	python3 -I scripts/bin2c.py $< $@

build/fonts.bin:
	scripts/make-fonts.sh

# the recompiler is a host tool sharing the decoder
build/tools/recomp: tools/recomp.c src/decode.c src/insn.h src/cpu.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ tools/recomp.c src/decode.c

# regenerate when the exe, the entry list or the recompiler changed
build/gen/index.c: $(EXE) $(wildcard tools/entries.txt) tools/recomp.c src/decode.c src/insn.h
	$(MAKE) build/tools/recomp
	@mkdir -p build/gen
	rm -f build/gen/seg_*.c build/gen/index.c
	build/tools/recomp $(EXE) build/gen $(wildcard tools/entries.txt)

gen: build/gen/index.c

recompile:
	rm -f build/gen/index.c
	$(MAKE) gen

clean:
	rm -rf build/host build/miyoo build/tools build/gen/seg_*.c build/gen/index.c

-include $(wildcard build/host/*.d build/miyoo/*.d)
