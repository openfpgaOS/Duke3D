#!/bin/bash
#
# openfpgaOS SDK — Standalone Game Core Builder
#
# Creates the complete SD card directory structure for deploying a game
# as an independent core on the Analogue Pocket.
#
# Usage:
#   ./buildcore.sh                 Interactive mode (prompts for each parameter)
#   ./buildcore.sh --batch ...     Non-interactive mode (all parameters via flags)
#

set -e

# ── Defaults ────────────────────────────────────────────────────────
AUTHOR="ThinkElastic"
PLATFORM=""
VERSION="1.0.0"
DATE=$(date +%Y-%m-%d)
SAVES=10
SAVE_SIZE="0x40000"
BATCH=0
ICON=""
OUTPUT=""
ELF=""
NAME=""
SHORT=""
SDK_ROOT="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="$SDK_ROOT/runtime"
BITSTREAM="$RUNTIME/bitstream.rbf_r"
OS_BIN="$RUNTIME/os.bin"
LOADER="$RUNTIME/loader.bin"
REVERSE_BITS="$RUNTIME/reverse_bits"
DATA_FILES=()

# ── Color helpers ───────────────────────────────────────────────────
GREEN='\033[92m'
RED='\033[91m'
YELLOW='\033[93m'
CYAN='\033[96m'
RESET='\033[0m'

ok()   { echo -e "  ${GREEN}✓${RESET} $1"; }
fail() { echo -e "  ${RED}✗${RESET} $1"; }
warn() { echo -e "  ${YELLOW}!${RESET} $1"; }

# ── Parse command-line args ─────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --batch)       BATCH=1; shift ;;
        --name)        NAME="$2"; shift 2 ;;
        --author)      AUTHOR="$2"; shift 2 ;;
        --short)       SHORT="$2"; shift 2 ;;
        --platform)    PLATFORM="$2"; shift 2 ;;
        --version)     VERSION="$2"; shift 2 ;;
        --elf)         ELF="$2"; shift 2 ;;
        --data)        DATA_FILES+=("$2"); shift 2 ;;
        --saves)       SAVES="$2"; shift 2 ;;
        --save-size)   SAVE_SIZE="$2"; shift 2 ;;
        --icon)        ICON="$2"; shift 2 ;;
        --output)      OUTPUT="$2"; shift 2 ;;
        --bitstream)   BITSTREAM="$2"; shift 2 ;;
        --os-bin)      OS_BIN="$2"; shift 2 ;;
        --loader)      LOADER="$2"; shift 2 ;;
        --date)        DATE="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--batch] [options]"
            echo "  Interactive mode (default) prompts for all parameters."
            echo "  Use --batch for scripted/CI usage with flags."
            echo ""
            echo "Options:"
            echo "  --name NAME         Game display name"
            echo "  --short SHORT       Short name, no spaces"
            echo "  --author AUTHOR     Core author [ThinkElastic]"
            echo "  --elf PATH          Path to game ELF binary"
            echo "  --data PATH         Data file (repeatable)"
            echo "  --saves N           Number of save slots [10]"
            echo "  --save-size HEX     Max save size per slot [0x40000]"
            echo "  --icon PATH         Core icon (.bin)"
            echo "  --output DIR        Output directory"
            echo "  --bitstream PATH    Source bitstream"
            echo "  --os-bin PATH       os.bin path"
            echo "  --loader PATH       Chip32 loader.bin path"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Derive short name from full name ───────────────────────────────
derive_short() {
    echo "$1" | sed 's/[^a-zA-Z0-9]//g'
}

# ── Prompt helper (with default) ───────────────────────────────────
ask() {
    local prompt="$1"
    local default="$2"
    local result
    if [[ -n "$default" ]]; then
        read -rp "$prompt [$default]: " result
        echo "${result:-$default}"
    else
        read -rp "$prompt: " result
        echo "$result"
    fi
}

# ── Validate file exists ──────────────────────────────────────────
check_file() {
    local path="$1"
    local label="$2"
    if [[ -f "$path" ]]; then
        local size=$(wc -c < "$path" | tr -d ' ')
        ok "$label: $path ($size bytes)"
        return 0
    else
        fail "$label: $path not found"
        return 1
    fi
}

# ── Interactive mode ───────────────────────────────────────────────
if [[ $BATCH -eq 0 ]]; then
    echo -e "${CYAN}=== openfpgaOS Core Builder ===${RESET}"
    echo

    NAME=$(ask "Game name" "$NAME")
    [[ -z "$NAME" ]] && { echo "Name is required."; exit 1; }

    default_short=$(derive_short "$NAME")
    SHORT=$(ask "Short name (no spaces)" "${SHORT:-$default_short}")
    AUTHOR=$(ask "Author" "$AUTHOR")
    VERSION=$(ask "Version" "$VERSION")
    [[ -z "$PLATFORM" ]] && PLATFORM=$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')
    PLATFORM=$(ask "Platform ID" "$PLATFORM")
    echo

    # ELF (optional — Enter to create a stub app)
    ELF=$(ask "ELF binary path (Enter to create stub)" "$ELF")
    if [[ -n "$ELF" ]]; then
        check_file "$ELF" "ELF" || { echo "File not found."; exit 1; }
    fi

    # Data files
    echo
    echo "Data files (one per line, empty line to finish):"
    DATA_FILES=()
    local_idx=1
    while true; do
        df=$(ask "  [$local_idx]" "")
        [[ -z "$df" ]] && break
        if [[ -f "$df" ]]; then
            ok "Found $(basename "$df")"
            DATA_FILES+=("$df")
            local_idx=$((local_idx + 1))
        else
            fail "$df not found, skipping"
        fi
    done
    echo "  ${#DATA_FILES[@]} data file(s) added."

    echo
    SAVES=$(ask "Number of save slots (0-10)" "$SAVES")
    if [[ $SAVES -gt 0 ]]; then
        SAVE_SIZE=$(ask "Max save size per slot" "$SAVE_SIZE")
    fi

    echo
    ICON=$(ask "Core icon path (optional, Enter to skip)" "$ICON")

    OUTPUT=$(ask "Output directory" "${OUTPUT:-dist/$SHORT}")

    # Summary
    echo
    echo -e "${CYAN}--- Summary ---${RESET}"
    echo "  Name:      $NAME"
    echo "  Short:     $SHORT"
    echo "  Author:    $AUTHOR"
    echo "  Version:   $VERSION"
    echo "  ELF:       $ELF"
    for df in "${DATA_FILES[@]}"; do
        echo "  Data:      $(basename "$df")"
    done
    echo "  Saves:     $SAVES × $SAVE_SIZE"
    echo "  Output:    $OUTPUT"
    echo

    read -rp "Proceed? [Y/n] " confirm
    [[ "$confirm" =~ ^[Nn] ]] && { echo "Aborted."; exit 0; }
fi

# ── Validate required inputs ───────────────────────────────────────
[[ -z "$NAME" ]] && { echo "Error: --name required"; exit 1; }
[[ -z "$ELF" ]]  && { echo "Error: --elf required"; exit 1; }
[[ -z "$SHORT" ]] && SHORT=$(derive_short "$NAME")
[[ -z "$PLATFORM" ]] && PLATFORM=$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')
[[ -z "$OUTPUT" ]] && OUTPUT="dist/$SHORT"

CORE_ID="${AUTHOR}.${SHORT}"
ELF_NAME=$(basename "$ELF")

echo
echo -e "${CYAN}Building core: $CORE_ID${RESET}"

# ── Check required source files ────────────────────────────────────
errors=0
check_file "$ELF" "ELF" || errors=1
check_file "$BITSTREAM" "Bitstream" || errors=1
check_file "$OS_BIN" "os.bin" || errors=1
check_file "$LOADER" "loader.bin" || errors=1
[[ $errors -ne 0 ]] && { echo "Missing required files."; exit 1; }


# ── Create directory structure ─────────────────────────────────────
CORE_DIR="$OUTPUT/Cores/$CORE_ID"
ASSETS_COMMON="$OUTPUT/Assets/$PLATFORM/common"
ASSETS_INSTANCE="$OUTPUT/Assets/$PLATFORM/$CORE_ID"
PLATFORMS_DIR="$OUTPUT/Platforms"
SAVES_DIR="$OUTPUT/Saves/$PLATFORM/common"

mkdir -p "$CORE_DIR" "$ASSETS_COMMON" "$PLATFORMS_DIR/_images"

# ── Generate core.json ─────────────────────────────────────────────
if [[ -f "$CORE_DIR/core.json" ]]; then
    ok "core.json already exists, skipping"
else
cat > "$CORE_DIR/core.json" << ENDJSON
{
    "core": {
        "magic": "APF_VER_1",
        "metadata": {
            "platform_ids": ["$PLATFORM"],
            "shortname": "$SHORT",
            "description": "$NAME on openfpgaOS",
            "author": "$AUTHOR",
            "url": "",
            "version": "$VERSION",
            "date_release": "$DATE"
        },
        "framework": {
            "target_product": "Analogue Pocket",
            "version_required": "2.2",
            "sleep_supported": false,
            "dock": { "supported": true, "analog_output": false },
            "hardware": { "link_port": true, "cartridge_adapter": 0 }
        },
        "cores": [
            { "name": "default", "id": 0, "filename": "bitstream.rbf_r", "chip32_vm": "loader.bin" }
        ]
    }
}
ENDJSON
ok "Generated core.json"
fi

# ── Generate data.json (filenames directly, no instance selector) ──
if [[ -f "$CORE_DIR/data.json" ]]; then
    ok "data.json already exists, skipping"
else
sname=$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')

DATA_SLOTS='[
            {
                "id": 1,
                "name": "OS Binary",
                "required": false,
                "parameters": 0,
                "filename": "os.bin",
                "extensions": ["bin"],
                "deferload": true
            },
            {
                "id": 2,
                "name": "Application",
                "required": false,
                "parameters": 0,
                "filename": "'"$ELF_NAME"'",
                "extensions": ["elf"],
                "deferload": true
            }'

# Add data file slots (3-6)
slot_id=3
for df in "${DATA_FILES[@]}"; do
    ext="${df##*.}"
    dfname=$(basename "$df")
    DATA_SLOTS="$DATA_SLOTS,"'
            {
                "id": '"$slot_id"',
                "name": "Data '"$((slot_id - 2))"'",
                "required": false,
                "parameters": 0,
                "filename": "'"$dfname"'",
                "extensions": ["'"$ext"'"],
                "deferload": true
            }'
    slot_id=$((slot_id + 1))
    [[ $slot_id -gt 6 ]] && { warn "Maximum 4 data slots (3-6), ignoring extra files"; break; }
done

# Add save slots
SAVE_ADDR=0x30000000
for i in $(seq 0 $((SAVES - 1))); do
    sid=$((10 + i))
    addr=$(printf "0x%08X" $SAVE_ADDR)
    DATA_SLOTS="$DATA_SLOTS,"'
            { "id": '"$sid"', "name": "Save '"$i"'", "required": false, "parameters": "0x85", "nonvolatile": true, "address": "'"$addr"'", "size_maximum": "'"$SAVE_SIZE"'", "filename": "'"${sname}_${i}.sav"'", "extensions": ["sav"] }'
    SAVE_ADDR=$((SAVE_ADDR + 0x40000))
done

cat > "$CORE_DIR/data.json" << ENDJSON
{
    "data": {
        "magic": "APF_VER_1",
        "data_slots": $DATA_SLOTS
        ]
    }
}
ENDJSON
ok "Generated data.json ($SAVES save slots, no instance needed)"
fi

# ── Copy shared JSON configs ──────────────────────────────────────
DIST_DIR=""
for try in "$SDK_ROOT/dist/sdk/core" "$SDK_ROOT/dist/sdk" "$RUNTIME/dist" dist ../openfpgaOS/dist; do
    if [[ -f "$try/audio.json" ]]; then
        DIST_DIR="$try"
        break
    fi
done

if [[ -n "$DIST_DIR" ]]; then
    for f in audio.json video.json input.json interact.json variants.json; do
        [[ -f "$DIST_DIR/$f" ]] && cp "$DIST_DIR/$f" "$CORE_DIR/"
    done
    ok "Copied shared JSON configs"

    # Platform files — copy existing or generate (don't overwrite)
    if [[ -f "$PLATFORMS_DIR/${PLATFORM}.json" ]]; then
        ok "Platform JSON already exists, skipping"
    elif [[ -f "$DIST_DIR/platforms/${PLATFORM}.json" ]]; then
        cp "$DIST_DIR/platforms/${PLATFORM}.json" "$PLATFORMS_DIR/"
        [[ -f "$DIST_DIR/platforms/_images/${PLATFORM}.bin" ]] && \
            cp "$DIST_DIR/platforms/_images/${PLATFORM}.bin" "$PLATFORMS_DIR/_images/"
        ok "Copied platform files"
    else
        # Generate platform JSON for standalone core
        cat > "$PLATFORMS_DIR/${PLATFORM}.json" << PLATJSON
{
    "platform": {
        "category": "Computer",
        "name": "$NAME",
        "year": $(date +%Y),
        "manufacturer": "$AUTHOR"
    }
}
PLATJSON
        ok "Generated platform: ${PLATFORM}.json"
    fi
else
    warn "dist/ not found — audio/video/input/interact/variants JSONs not copied"
fi

# ── Copy bitstream ─────────────────────────────────────────────────
cp "$BITSTREAM" "$CORE_DIR/bitstream.rbf_r"
ok "Copied bitstream.rbf_r"

# ── Copy loader, OS, ELF, data files ──────────────────────────────
cp "$LOADER" "$CORE_DIR/loader.bin"
ok "Copied loader.bin"

cp "$OS_BIN" "$ASSETS_COMMON/os.bin"
ok "Copied os.bin"

cp "$ELF" "$ASSETS_COMMON/$ELF_NAME"
ok "Copied $ELF_NAME"

for df in "${DATA_FILES[@]}"; do
    cp "$df" "$ASSETS_COMMON/$(basename "$df")"
    ok "Copied $(basename "$df")"
done

# ── Copy icon if provided ─────────────────────────────────────────
if [[ -n "$ICON" && -f "$ICON" ]]; then
    cp "$ICON" "$CORE_DIR/icon.bin"
    ok "Copied icon"
fi

# ── Create stub app source if it doesn't exist ───────────────────
APP_SRC_DIR="$(cd "$(dirname "$0")" && pwd)/src/$(echo "$SHORT" | tr '[:upper:]' '[:lower:]')"
if [[ ! -d "$APP_SRC_DIR" ]]; then
    mkdir -p "$APP_SRC_DIR"
    cat > "$APP_SRC_DIR/main.c" << 'STUBEOF'
/*
 * Hello World — openfpgaOS stub app
 *
 * Build with: make
 */

#include "of.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    of_video_init();

    /* Set a simple grayscale palette */
    for (int i = 0; i < 256; i++)
        of_video_palette(i, (i << 16) | (i << 8) | i);

    /* Draw a gradient */
    uint8_t *fb = of_video_surface();
    for (int y = 0; y < 240; y++)
        memset(&fb[y * 320], y, 320);

    of_video_flip();

    printf("Hello from openfpgaOS!\n");
    printf("Press any button...\n");

    while (1) {
        of_input_poll();
        of_delay_ms(16);
    }

    return 0;
}
STUBEOF
    ok "Created stub app: src/apps/$(basename "$APP_SRC_DIR")/main.c"
    echo "  Edit it, run 'make', then 'make deploy'"
fi

# ── Summary ────────────────────────────────────────────────────────
echo
echo -e "${GREEN}Core configured!${RESET}"
echo "  Core output: $OUTPUT/"
echo "  App source:  src/apps/$(basename "$APP_SRC_DIR")/main.c"
echo
echo "Next steps:"
echo "  1. Edit src/apps/$(basename "$APP_SRC_DIR")/main.c"
echo "  2. make"
echo "  3. make deploy"
