#!/bin/bash
#
# openfpgaOS SDK — Create New App
#
# Scaffolds a new app with its own Makefile, stub source, and instance JSON.
# Core configs (data.json, audio.json, etc.) are SDK-owned — your app only
# maintains an instance.json that maps filenames to data slots.
#
# Usage:
#   ./scripts/new.sh mygame                  Create src/mygame/
#   ./scripts/new.sh mygame --target pocket  Target platform (default: pocket)
#   Called by scripts/customize.sh (make core)
#

set -e

SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORMS_DIR="$SDK_ROOT/src/sdk/platforms"

GREEN='\033[92m'
RED='\033[91m'
CYAN='\033[96m'
RESET='\033[0m'

ok()   { echo -e "  ${GREEN}+${RESET} $1"; }
fail() { echo -e "  ${RED}x${RESET} $1"; exit 1; }

# ── Parse args ───────────────────────────────────────────────────────
APP=""
TARGET="pocket"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target|-t) TARGET="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 <app_name> [--target pocket|mister]"
            echo "Creates src/<app_name>/ with Makefile, main.c, and instance.json."
            exit 0 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *)  APP="$1"; shift ;;
    esac
done

[[ -z "$APP" ]] && { echo "Usage: $0 <app_name> [--target pocket|mister]"; exit 1; }

# Sanitize: lowercase, no spaces
APP_LOWER=$(echo "$APP" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9_]//g')
[[ -z "$APP_LOWER" ]] && fail "Invalid app name: $APP"

APP_DIR="$SDK_ROOT/src/$APP_LOWER"

# ── Validate target platform ────────────────────────────────────────
if [[ ! -d "$PLATFORMS_DIR/$TARGET" ]]; then
    fail "Unknown target: $TARGET"
    echo "  Available targets:"
    for d in "$PLATFORMS_DIR"/*/; do
        echo "    $(basename "$d")"
    done
    exit 1
fi

TEMPLATES="$PLATFORMS_DIR/$TARGET/templates"

# ── Check if app already exists ──────────────────────────────────────
if [[ -d "$APP_DIR" ]]; then
    fail "Directory already exists: src/$APP_LOWER/"
fi

echo -e "${CYAN}Creating app: $APP_LOWER${RESET} (target: $TARGET)"
echo

# ── Create app directory ─────────────────────────────────────────────
mkdir -p "$APP_DIR"

# ── Generate Makefile ──────────────────────────────────────────��─────
cat > "$APP_DIR/Makefile" << 'MKEOF'
# {{APP_DISPLAY}} — openfpgaOS App
#
# Targets:
#   make            Build app
#   make exec       Build, push via UART, stream console
#   make deploy     Deploy to hardware (auto-detects SD card)
#   make package    Create distributable ZIP
#   make pc         Build for desktop (SDL2)
#   make clean      Remove build artifacts
#

APP       = {{APP}}
TARGET    = {{TARGET}}
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

SRCS     = $(wildcard *.c)
SRCS_CXX = $(wildcard *.cpp)

# Declare default target before sdk.mk's rules
.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

$(CRT_DIR)/start.o: $(CRT_DIR)/start.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Build ELF, then assemble: dist/<app>/ + runtime + ELF = deployable
all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

release: $(OBJ_DIR)/app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d $(OUT)/Cores/*/)/
	@mkdir -p $$(ls -d $(OUT)/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d $(OUT)/Assets/*/)/common/
	@cp $< $$(ls -d $(OUT)/Assets/*/)/common/$(APP).elf
	@for f in *.mid *.mod *.wav *.dat *.png; do [ -f "$$f" ] && cp "$$f" $$(ls -d $(OUT)/Assets/*/)/common/; done 2>/dev/null; true
	@echo "Ready: build/$(APP)/"

COMMON = $(shell ls -d $(OUT)/Assets/*/common 2>/dev/null)

exec: all
	@$(ROOT)/scripts/exec.sh $$(ls -d $(OUT)/Assets/*/common)/$(APP).elf

deploy: all
	@$(SDK_DIR)/platforms/$(TARGET)/deploy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

pc: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release exec deploy package pc clean
MKEOF

# Fill in placeholders
sed -i "s/{{APP}}/$APP_LOWER/g; s/{{APP_DISPLAY}}/$APP/g; s/{{TARGET}}/$TARGET/g" "$APP_DIR/Makefile"
ok "Makefile"

# ── Generate main.c ──────────────────────────────────────────────────
cat > "$APP_DIR/main.c" << 'CEOF'
/*
 * {{APP_DISPLAY}} — openfpgaOS app
 *
 * Build:   make
 * Deploy:  make deploy
 * PC test: make pc
 */

#include "of.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    of_video_init();

    /* Set up a gradient palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | ((255 - i) << 8) | 128);

    /* Draw something */
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            fb[y * 320 + x] = x ^ y;

    of_video_flip();

    printf("Hello from {{APP_DISPLAY}}!\n");

    /* Main loop */
    while (1) {
        of_input_poll();

        if (of_btn_pressed(OF_BTN_A)) {
            /* A button pressed */
        }

        usleep(16000);  /* ~60 fps */
    }
}
CEOF

sed -i "s/{{APP_DISPLAY}}/$APP/g" "$APP_DIR/main.c"
ok "main.c"

# ── Summary ──────────────────────────────────────────────────────────
echo
echo -e "${GREEN}Done!${RESET} Your app is at: src/$APP_LOWER/"
echo
echo "  cd src/$APP_LOWER"
echo "  make              # build"
echo "  make deploy       # deploy to Pocket SD card"
echo "  make pc           # test on desktop (SDL2)"
echo
echo "Edit main.c to start building your app."
