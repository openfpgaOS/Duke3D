# openfpgaOS SDK — Build Rules
#
# This file is SDK-owned. Do not edit — it gets replaced on SDK updates.
# Customize your build in Makefile instead.
#
# SDK version: 3
#

# ── Toolchain (auto-detect) ───────────────────────────────────────
CROSS ?= $(shell which riscv64-unknown-elf-gcc >/dev/null 2>&1 && echo riscv64-unknown-elf- || echo riscv64-elf-)
CC      = $(CROSS)gcc
LD      = $(CROSS)gcc
AS      = $(CROSS)gcc
OBJDUMP = $(CROSS)objdump
SIZE    = $(CROSS)size

# ── Architecture ──────────────────────────────────────────────────
ARCH = rv32imafc
ABI  = ilp32f

# ── Paths ─────────────────────────────────────────────────────────
SDK_DIR ?= src/sdk
CRT_DIR  = $(SDK_DIR)/crt
APP_LD  ?= $(SDK_DIR)/crt/app.ld

# ── Compiler flags ────────────────────────────────────────────────
SDK_CFLAGS  = -march=$(ARCH) -mabi=$(ABI) -O2 -Wall -Wextra
SDK_CFLAGS += -ffreestanding -nostdlib -nostartfiles
SDK_CFLAGS += -ffunction-sections -fdata-sections
SDK_CFLAGS += -fno-builtin
SDK_CFLAGS += -nostdinc -I$(SDK_DIR)/libc -I$(SDK_DIR)/include -I.
SDK_CFLAGS += -isystem $(shell $(CC) -print-file-name=include)

CFLAGS ?=
ALL_CFLAGS = $(SDK_CFLAGS) $(CFLAGS)

SDK_LDFLAGS  = -march=$(ARCH) -mabi=$(ABI)
SDK_LDFLAGS += -nostdlib -nostartfiles -static
SDK_LDFLAGS += -T $(APP_LD) -Wl,--gc-sections

LDFLAGS ?=
ALL_LDFLAGS = $(SDK_LDFLAGS) $(LDFLAGS)

ASFLAGS = -march=$(ARCH)_zicsr -mabi=$(ABI)

LIBGCC = $(shell $(CC) -march=$(ARCH) -mabi=$(ABI) -print-libgcc-file-name)

# ── Objects ───────────────────────────────────────────────────────
CRT_START = $(CRT_DIR)/start.o
CRT_POSIX = $(CRT_DIR)/of_posix.o
CRT_MIDI  = $(CRT_DIR)/of_midi.o
APP_OBJS  = $(SRCS:.c=.o)
OBJS      = $(CRT_START) $(CRT_POSIX) $(CRT_MIDI) $(APP_OBJS)

# ── Pocket build ─────────────────────────────────────────────────
app.elf: $(OBJS) $(APP_LD)
	$(LD) $(ALL_LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

$(CRT_DIR)/%.o: $(CRT_DIR)/%.S
	$(AS) $(ASFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

# ── PC build (SDL2) ──────────────────────────────────────────────
PC_CC ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null)

app_pc: $(SRCS) $(SDK_DIR)/pc/of_sdl2.c $(SDK_DIR)/include/of.h
	$(PC_CC) -DOF_PC -I$(SDK_DIR)/include -I. -O2 -Wall -Wextra \
		$(SRCS) $(SDK_DIR)/pc/of_sdl2.c \
		$(SDL_CFLAGS) $(SDL_LIBS) -lm -o $@

# ── Shared clean rule (preserves pre-built CRT objects) ─────────
sdk-clean:
	rm -f $(APP_OBJS) app.elf app_pc
