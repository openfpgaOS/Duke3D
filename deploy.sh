#!/bin/bash
#
# openfpgaOS SDK — Deploy to SD Card
#
# Deploys the openfpgaOS core with all bundled apps + your app to the SD card.
# Auto-detects the SD card or uses the path provided.
#
# Usage:
#   ./deploy.sh                     Auto-detect SD card
#   ./deploy.sh /mnt/sdcard         Deploy to specific path
#

set -e

GREEN='\033[92m'
CYAN='\033[96m'
YELLOW='\033[93m'
RESET='\033[0m'

SDK_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="$SDK_DIR/runtime"
CORE_ID="ThinkElastic.openfpgaOS"
PLATFORM="openfpgaos"

# ── Find SD card ───────────────────────────────────────────────────
find_sd() {
    for mount in /run/media/"$USER"/*; do
        if [ -d "$mount/Cores" ] && [ -d "$mount/Assets" ]; then
            echo "$mount"
            return
        fi
    done
}

SDCARD="$1"
if [ -z "$SDCARD" ]; then
    SDCARD="$(find_sd)"
    if [ -z "$SDCARD" ]; then
        echo "Error: No openfpgaOS SD card found"
        echo "Usage: $0 [/path/to/sdcard]"
        exit 1
    fi
fi

echo -e "${CYAN}Deploying to: $SDCARD${RESET}"

# ── Build all apps first ──────────────────────────────────────────
echo "Building apps..."
if ! make -C "$SDK_DIR/src/apps" 2>&1 | grep -v "^make\|^$"; then
    echo -e "  ${YELLOW}!${RESET} Some apps may have failed to build"
fi

# ── Validate runtime ──────────────────────────────────────────────
if [ ! -f "$RUNTIME/bitstream.rbf_r" ]; then
    echo "Error: runtime/bitstream.rbf_r not found. Run ./publish.sh from openfpgaOS, or download runtime from releases."
    exit 1
fi

# ── Create directories ────────────────────────────────────────────
CORE_DIR="$SDCARD/Cores/$CORE_ID"
ASSETS_COMMON="$SDCARD/Assets/$PLATFORM/common"
ASSETS_INSTANCE="$SDCARD/Assets/$PLATFORM/$CORE_ID"
PLATFORMS_DIR="$SDCARD/Platforms"

mkdir -p "$CORE_DIR" "$ASSETS_COMMON" "$ASSETS_INSTANCE" "$PLATFORMS_DIR/_images"

# ── Core files ────────────────────────────────────────────────────
cp "$RUNTIME/bitstream.rbf_r" "$CORE_DIR/"
cp "$RUNTIME/loader.bin" "$CORE_DIR/"
echo -e "  ${GREEN}✓${RESET} Bitstream + loader"

# Core configs (from dist/sdk/core/ if present)
if [ -d "$SDK_DIR/dist/sdk/core" ]; then
    for f in "$SDK_DIR/dist/sdk/core"/*.json "$SDK_DIR/dist/sdk/core"/*.bin; do
        [ -f "$f" ] && cp "$f" "$CORE_DIR/"
    done
    echo -e "  ${GREEN}✓${RESET} Core configs"
fi

# Platform files (from dist/sdk/platform/ if present)
if [ -d "$SDK_DIR/dist/sdk/platform" ]; then
    [ -f "$SDK_DIR/dist/sdk/platform/${PLATFORM}.json" ] && \
        cp "$SDK_DIR/dist/sdk/platform/${PLATFORM}.json" "$PLATFORMS_DIR/"
    [ -f "$SDK_DIR/dist/sdk/platform/_images/${PLATFORM}.bin" ] && \
        cp "$SDK_DIR/dist/sdk/platform/_images/${PLATFORM}.bin" "$PLATFORMS_DIR/_images/"
    echo -e "  ${GREEN}✓${RESET} Platform files"
fi

# ── OS binary ─────────────────────────────────────────────────────
cp "$RUNTIME/os.bin" "$ASSETS_COMMON/"
echo -e "  ${GREEN}✓${RESET} os.bin"

# ── Bundled apps (built from source in apps/) ────────────────────
APP_COUNT=0
for appdir in "$SDK_DIR/src/apps"/*/; do
    [ -d "$appdir" ] || continue
    appname=$(basename "$appdir")
    if [ -f "$appdir/app.elf" ]; then
        cp "$appdir/app.elf" "$ASSETS_COMMON/${appname}.elf"
        APP_COUNT=$((APP_COUNT + 1))
    fi
    # Copy app data files
    for f in "$appdir"/*.mid "$appdir"/*.wav "$appdir"/*.dat "$appdir"/*.png; do
        [ -f "$f" ] && cp "$f" "$ASSETS_COMMON/"
    done
done
echo -e "  ${GREEN}✓${RESET} Bundled apps ($APP_COUNT)"

# Clean stale instance JSONs from SD card
rm -f "$ASSETS_INSTANCE"/*.json 2>/dev/null

# Bundled instance JSONs — only deploy if the app ELF exists
INST_COUNT=0
if [ -d "$SDK_DIR/dist/sdk/instances" ]; then
    for inst in "$SDK_DIR/dist/sdk/instances"/*.json; do
        [ -f "$inst" ] || continue
        # Extract ELF filename from instance JSON (slot id 2)
        elf_name=$(python3 -c "
import json,sys
d=json.load(open('$inst'))
for s in d['instance']['data_slots']:
    if s['id']==2: print(s['filename']); break
" 2>/dev/null)
        if [ -n "$elf_name" ] && [ -f "$ASSETS_COMMON/$elf_name" ]; then
            cp "$inst" "$ASSETS_INSTANCE/"
            INST_COUNT=$((INST_COUNT + 1))
        fi
    done
    echo -e "  ${GREEN}✓${RESET} Instance JSONs ($INST_COUNT)"
fi


# ── Standalone game cores (build/<name>/ excluding sdk/) ─────────
for coredir in "$SDK_DIR/build"/*/; do
    name=$(basename "$coredir")
    [ "$name" = "sdk" ] && continue
    [ ! -d "$coredir/Cores" ] && continue
    cp -r "$coredir"/* "$SDCARD/"
    echo -e "  ${GREEN}✓${RESET} Standalone core: $name"
done

# ── Sync ──────────────────────────────────────────────────────────
sync

echo -e "\n${GREEN}Deploy complete!${RESET}"
echo "  $CORE_DIR/"
