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
TARGET   ?= pocket
MISTER_IP ?= mister.local
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/Duke3D
OUT       = $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME   = $(ROOT)/runtime
# Game data files shipped into the per-target deliverable, when present
# alongside the Makefile (the GRP is usually user-supplied).
GAME_ASSETS = $(wildcard *.dat *.bin *.cfg)

# ── Extra flags for BUILD engine ─────────────────────────────────
CFLAGS += -O3
CFLAGS += -DOPENFPGA -DUNIX
CFLAGS += -DSMP_MAX_VOICES=20
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

# Per-target deliverable: pocket = APF SD tree, mister = disk image.
# The platform owns the layout (image.sh); this rule names no target.
ifeq ($(TARGET),mister)
release: game-dist        # MiSTer: the per-game bundle IS the release artifact
else
release: $(OBJ_DIR)/app.elf
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(GAME_ASSETS)
endif

# DEBUG=v / DEBUG=t forwards -v / -t to phdpd via debug.sh.
DEBUG ?=
DEBUG_FLAGS = $(if $(DEBUG),-$(DEBUG))

debug: all
	@$(ROOT)/scripts/debug.sh $(DEBUG_FLAGS) "$$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf"

# MISTER_IP rides the environment so the same call serves every platform.
copy: all
	@MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$(OBJ_DIR)/app.elf"


# ── MiSTer Downloader DB (same scheme as the Doom repo) ──────────────
MISTER_DB_ID   ?= openfpgaOS/openfpgaos-duke3d
MISTER_DB_VER  := $(shell python3 -c "import glob,json;print(json.load(open(sorted(glob.glob('$(DIST)/Cores/*/core.json'))[0]))['core']['metadata']['version'])" 2>/dev/null || echo 0.0.0)
# Asset URLs pin to this release's own immutable tag; the DB itself is
# committed and served via raw — a stable db_url immune to the per-repo
# releases/latest pointer.
MISTER_DB_BASE_URL ?= https://github.com/openfpgaOS/Duke3D/releases/download/duke3d-mister-v$(MISTER_DB_VER)/
MISTER_DB_URL      ?= https://raw.githubusercontent.com/openfpgaOS/Duke3D/main/releases/mister/duke3d.json.zip
MISTER_DB          = $(ROOT)/releases/mister/$(APP).json.zip
MISTER_DB_INI      = $(ROOT)/releases/mister/$(APP).downloader.ini

package: export ASSET_LABEL    = the commercial GRP
package: export ASSET_EXAMPLES = duke3d.grp
ifeq ($(TARGET),mister)
package: game-dist
	@$(ROOT)/scripts/package.sh $(APP)
	@$(SDK_DIR)/platforms/mister/mkdb.py \
	    --staging "$(GAME_STAGE)" --sd-prefix "games/OpenfpgaOS" \
	    --menu-dir "_Computer/_OpenfpgaOS/_$(GAME)" \
	    --base-url "$(MISTER_DB_BASE_URL)" --url-mode flat --db-id "$(MISTER_DB_ID)" \
	    --db-url "$(MISTER_DB_URL)" \
	    --output "$(MISTER_DB)" --ini-out "$(MISTER_DB_INI)"
else
package: all
	@$(ROOT)/scripts/package.sh $(APP)
endif

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package clean

# ── MiSTer per-game locked distribution ──────────────────────────────
# Mirrors src/doom/Makefile: read-only boot.vhd shell (the data files), a
# writable saves volume, one .mgl per instance and the loose F-loaded engine.
GAME        = Duke3D
GAME_INIS   = $(DIST)/Assets/duke3d/common
GAME_STAGE  = $(ROOT)/build/mister/$(APP)
GAME_COMMON = $(ROOT)/build/mister/.$(APP)-common
GAME_ELF    = duke3d.elf
GAME_CSV    = $(DIST)/mister/external_files.csv
# Explicit shell size: duke3d.grp alone is 44 MB — the 48 MB FAT32 floor was
# already borderline, and add-on GRPs (Caribbean/DC/Vacation) need real room.
# Ships gzipped, so the size is nearly free.
GAME_BOOT_MB = 256

game-dist: $(OBJ_DIR)/app.elf
	@rm -rf "$(GAME_STAGE)" "$(GAME_COMMON)"
	@mkdir -p "$(GAME_STAGE)/$(GAME)/wads" "$(GAME_COMMON)"
	@cp "$(RUNTIME)/bank.ofsf" "$(GAME_COMMON)/bank.ofsf"
	@$(SDK_DIR)/platforms/mister/mkgame.sh --no-elf --saves-out "$(GAME_STAGE)/$(GAME)/$(GAME).saves.vhd" "$(GAME)" "$(GAME_INIS)" "$(GAME_COMMON)" "$(GAME_STAGE)/$(GAME)/boot.vhd" $(GAME_BOOT_MB)
	@gzip -9nf "$(GAME_STAGE)/$(GAME)"/boot*.vhd
	@$(SDK_DIR)/platforms/mister/mkmgl.sh "$(GAME)" "$(GAME_INIS)" "$(GAME_STAGE)"
	@cp "$(OBJ_DIR)/app.elf" "$(GAME_STAGE)/$(GAME)/$(GAME_ELF)"
	@cp "$(GAME_INIS)"/*.ini "$(GAME_STAGE)/$(GAME)/"
	@sed 's|^GAME_DEFAULT=.*|GAME_DEFAULT="$(GAME)"|' "$(SDK_DIR)/platforms/mister/setup.sh" > "$(GAME_STAGE)/$(GAME)/setup.sh"
	@chmod +x "$(GAME_STAGE)/$(GAME)/setup.sh"
	@if [ -f "$(GAME_CSV)" ]; then cp "$(GAME_CSV)" "$(GAME_STAGE)/$(GAME)/external_files.csv"; fi
	@rm -rf "$(GAME_COMMON)"
	@echo "  game-dist staged -> $(GAME_STAGE)"
