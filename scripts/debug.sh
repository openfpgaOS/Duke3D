#!/bin/bash
#
# openfpgaOS SDK — Quick Deploy & Run
#
# Pushes a binary to the Pocket over UART, resets the core, and
# streams console output until Ctrl+C.
#
# Usage:
#   ./scripts/exec.sh path/to/app.elf        Push to slot 2 (Application)
#   ./scripts/exec.sh path/to/os.bin         Push to slot 1 (OS)
#   ./scripts/exec.sh --slot N path/to/file  Push to explicit slot
#

set -e

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PHDPD="$SDK_DIR/src/tools/phdp/phdpd"
PHDP="$SDK_DIR/src/tools/phdp/phdp"

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'

# ── Parse arguments ────────────────────────────────────────────────
SLOT=""
FILE=""

if [ "$1" = "--slot" ]; then
    SLOT="$2"
    FILE="$3"
else
    FILE="$1"
fi

if [ -z "$FILE" ]; then
    echo -e "${RED}Usage: $0 [--slot N] <file>${RESET}"
    exit 1
fi

if [ ! -f "$FILE" ]; then
    echo -e "${RED}File not found: $FILE${RESET}"
    exit 1
fi

# ── Auto-detect slot from filename if not specified ────────────────
if [ -z "$SLOT" ]; then
    case "$(basename "$FILE")" in
        os.bin)  SLOT=1 ;;
        *)       SLOT=2 ;;
    esac
fi

# ── Ensure phdpd is running ───────────────────────────────────────
if ! pgrep -x phdpd >/dev/null 2>&1; then
    echo -e "${GREEN}Starting phdpd...${RESET}"
    "$PHDPD" &
    for i in $(seq 1 20); do
        [ -S /tmp/phdp.sock ] && break
        sleep 0.1
    done
    if ! pgrep -x phdpd >/dev/null 2>&1; then
        echo -e "${RED}Failed to start phdpd${RESET}"
        exit 1
    fi
fi

# ── Clear pending slots ───────────────────────────────────────────
"$PHDP" clear

# ── Push file to slot ─────────────────────────────────────────────
"$PHDP" push --slot "$SLOT" "$FILE"

# ── Reset core and wait for OS boot ───────────────────────────────
"$PHDP" reset
"$PHDP" wait
"$PHDP" logs
