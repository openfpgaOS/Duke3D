# DukeNukem3D — Duke Nukem 3D for openfpgaOS
#
# Build from SDK root:
#   make build CORE=duke3d
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

SHIM_SRCS = posix_shim.c d3d_audio.c d3d_save.c d3d_gpu.c

# SDK helper sources -- duke3d's midi_of.c calls of_midi_* which lives
# in the SDK's of_midi.c.  The MIDI engine now delegates note rendering
# to the sample-voice engine (smp_voice), which reads the auto-loaded
# SoundFont via of_smp_bank + of_smp_tables, so all four translation
# units have to come along.
SDK_SRCS = $(OF_MIDI_SRC) \
           $(SDK_DIR)/of_smp_bank.c \
           $(SDK_DIR)/of_smp_voice.c \
           $(SDK_DIR)/of_smp_tables.c

SRCS = $(ENGINE_SRCS) $(GAME_SRCS) $(AUDIOLIB_SRCS) $(CONSOLE_SRCS) $(MIDI_SRCS) $(SHIM_SRCS) $(SDK_SRCS)

# ── SDK paths ────────────────────────────────────────────────────
APP       = duke3d
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/Duke3D
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

# ── Extra flags for BUILD engine ─────────────────────────────────
CFLAGS += -O3
CFLAGS += -DOPENFPGA -DUNIX
CFLAGS += -DSMP_MAX_VOICES=12
CFLAGS += -IEngine/src -IGame/src -IGame/src/audiolib -I.
CFLAGS += -fno-strict-aliasing -fcommon
CFLAGS += -std=gnu11 -Wno-error -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-missing-field-initializers
CFLAGS += -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-pointer-sign
CFLAGS += -Wno-unused-but-set-variable -Wno-tautological-compare -Wno-char-subscripts
CFLAGS += -Wno-address -Wno-array-bounds -Wno-aggressive-loop-optimizations
CFLAGS += -Wno-discarded-qualifiers -Wno-unused-function -Wno-array-parameter
CFLAGS += -Wno-maybe-uninitialized -Wno-empty-body

# ── SDK build rules ──────────────────────────────────────────────
.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

# Assemble SD card image: dist/Duke3D/ + runtime + ELF → build/duke3d/
release: $(OBJ_DIR)/app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d $(OUT)/Cores/*/)/
	@mkdir -p $$(ls -d $(OUT)/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d $(OUT)/Assets/*/)/common/
	@# SoundFont bank — auto-loaded by the kernel and used by of_midi
	@cp $(RUNTIME)/bank.ofsf $$(ls -d $(OUT)/Assets/*/)/common/
	@cp $< $$(ls -d $(OUT)/Assets/*/)/common/$(APP).elf
	@echo "Ready: build/$(APP)/"

# DEBUG=v / DEBUG=t forwards -v / -t to phdpd via debug.sh.
DEBUG ?=
DEBUG_FLAGS = $(if $(DEBUG),-$(DEBUG))

debug: all
	@$(ROOT)/scripts/debug.sh $(DEBUG_FLAGS) $$(find $(OUT)/Assets -name "$(APP).elf" | head -1)

copy: all
	@$(SDK_DIR)/platforms/pocket/copy.sh "$(APP)" "$$(find $(OUT)/Assets -name '$(APP).elf' | head -1)"

package: all
	@$(ROOT)/scripts/package.sh Duke3D

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package clean
