#!/bin/bash
#
# openfpgaOS SDK — Deploy to Pocket SD Card
#
# Copies build/sdk/ (the complete SD card image) to the mounted SD card.
# Run 'make' from src/apps/ first to assemble build/sdk/.
#
# Usage:
#   ./scripts/deploy.sh                     Auto-detect Pocket SD card
#   ./scripts/deploy.sh /mnt/sdcard         Deploy to specific path
#

set -e

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$SDK_DIR/build/sdk"

# ── Check build exists ───────────────────────────────────────────────
if [ ! -d "$BUILD_DIR/Cores" ]; then
    echo "Error: build/sdk/ not found. Run 'make' from src/apps/ first."
    exit 1
fi

# ── Find SD card ───────────────────────────────────────────────────
find_pocket_sd() {
    for mount in /run/media/"$USER"/* /Volumes/*; do
        if [ -d "$mount/Cores" ] && [ -d "$mount/Assets" ]; then
            echo "$mount"
            return
        fi
    done
}

SDCARD="$1"
if [ -z "$SDCARD" ]; then
    SDCARD="$(find_pocket_sd)"
    if [ -z "$SDCARD" ]; then
        echo "Error: No Analogue Pocket SD card found"
        echo "Usage: $0 [/path/to/sdcard]"
        exit 1
    fi
fi

echo "Deploying build/sdk/ to $SDCARD"

# ── Copy ─────────────────────────────────────────────────────────────
cp -r "$BUILD_DIR"/Cores/* "$SDCARD/Cores/" 2>/dev/null
echo -e "  ${GREEN}+${RESET} Cores"

cp -r "$BUILD_DIR"/Assets/* "$SDCARD/Assets/" 2>/dev/null
echo -e "  ${GREEN}+${RESET} Assets"

cp -r "$BUILD_DIR"/Platforms/* "$SDCARD/Platforms/" 2>/dev/null
echo -e "  ${GREEN}+${RESET} Platforms"

sync 2>/dev/null || true
echo -e "\n${GREEN}Deployed!${RESET} Eject SD card and boot your Pocket."
