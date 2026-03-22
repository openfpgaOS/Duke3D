# DukeNukem3D — Duke Nukem 3D for openfpgaOS
#
# Build:   make -C src/duke3d -f Makefile
# or from SDK root: make (builds this + bundled apps)
#

# ── Sources ──────────────────────────────────────────────────────
ENGINE_SRCS = \
	Engine/src/cache.c \
	Engine/src/display_of.c \
	Engine/src/draw.c \
	Engine/src/engine.c \
	Engine/src/filesystem.c \
	Engine/src/fixedPoint_math.c \
	Engine/src/dummy_multi.c \
	Engine/src/tiles.c

GAME_SRCS = \
	Game/src/actors.c \
	Game/src/animlib.c \
	Game/src/config.c \
	Game/src/control.c \
	Game/src/game.c \
	Game/src/gamedef.c \
	Game/src/global.c \
	Game/src/keyboard.c \
	Game/src/menues.c \
	Game/src/player.c \
	Game/src/premap.c \
	Game/src/rts.c \
	Game/src/scriplib.c \
	Game/src/sector.c \
	Game/src/sounds.c

AUDIOLIB_SRCS = \
	Game/src/audiolib/dpmi.c \
	Game/src/audiolib/fx_man.c \
	Game/src/audiolib/ll_man.c \
	Game/src/audiolib/multivoc.c \
	Game/src/audiolib/mv_mix.c \
	Game/src/audiolib/mvreverb.c \
	Game/src/audiolib/pitch.c

CONSOLE_SRCS = \
	Game/src/console.c \
	Game/src/cvar_defs.c \
	Game/src/cvars.c

MIDI_SRCS = Game/src/midi/midi_of.c

SHIM_SRCS = of_posix.c posix_shim.c d3d_audio.c d3d_save.c

SRCS = $(ENGINE_SRCS) $(GAME_SRCS) $(AUDIOLIB_SRCS) $(CONSOLE_SRCS) $(MIDI_SRCS) $(SHIM_SRCS)

# ── SDK paths ────────────────────────────────────────────────────
SDK_DIR = ../sdk

# ── Extra flags for BUILD engine ─────────────────────────────────
CFLAGS += -DOPENFPGA -DUNIX
CFLAGS += -IEngine/src -IGame/src -IGame/src/audiolib -I.
CFLAGS += -fno-strict-aliasing -fcommon
CFLAGS += -std=gnu11 -w -Wno-error -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration

# ── SDK build rules ──────────────────────────────────────────────
include $(SDK_DIR)/sdk.mk

# Explicit CRT rule (pattern rule doesn't work across directories)
$(CRT_DIR)/start.o: $(CRT_DIR)/start.S
	$(AS) $(ASFLAGS) -c -o $@ $<

all: app.elf
	$(SIZE) $<

clean: sdk-clean

.PHONY: all clean
