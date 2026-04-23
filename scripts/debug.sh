#!/bin/bash
#
# openfpgaOS SDK — Debug (UART)
#
# Pushes a binary to the Pocket over UART, resets the core, and
# streams console output until Ctrl+C.
#
# Usage:
#   ./scripts/debug.sh path/to/app.elf       Push to slot 2 (Application)
#   ./scripts/debug.sh path/to/os.bin        Push to slot 1 (OS)
#   ./scripts/debug.sh --slot N path/to/file Push to explicit slot
#   ./scripts/debug.sh -v ...                Forward -v to phdpd (state + packet headers)
#   ./scripts/debug.sh -t ...                Forward -t to phdpd (full hex trace + IPC events)
#
# If phdpd is already running with a different verbosity than requested,
# this script restarts it so the new flag takes effect.
#

set -e

SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PHDPD="$SDK_DIR/src/tools/phdp/phdpd"
PHDP="$SDK_DIR/src/tools/phdp/phdp"

# ── Build host tools on demand ─────────────────────────────────────
# Neither the root `make debug` target nor the per-app debug rules
# depend on the phdp tools, so this script must make sure they exist
# before trying to invoke them.
if [ ! -x "$PHDPD" ] || [ ! -x "$PHDP" ]; then
    echo -e "\033[92mBuilding phdp tools...\033[0m"
    if ! make -C "$SDK_DIR/src/tools/phdp" >/dev/null; then
        echo -e "\033[91mFailed to build phdp tools\033[0m"
        exit 1
    fi
fi

GREEN='\033[92m'
RED='\033[91m'
RESET='\033[0m'

# ── Parse arguments ────────────────────────────────────────────────
SLOT=""
FILE=""
PHDPD_FLAGS=""

# Strip optional -v/-t before the slot/file args.
while [ $# -gt 0 ]; do
    case "$1" in
        -v) PHDPD_FLAGS="$PHDPD_FLAGS -v"; shift ;;
        -t) PHDPD_FLAGS="$PHDPD_FLAGS -t"; shift ;;
        *)  break ;;
    esac
done

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

# ── Ensure phdpd is running with the requested verbosity ─────────
# If verbosity flags were requested AND phdpd is already up, restart
# it so the new flag takes effect — there's no IPC channel for
# changing verbosity at runtime, and "running with -v" vs "running
# silent" produce very different debug output.
if [ -n "$PHDPD_FLAGS" ] && pgrep -x phdpd >/dev/null 2>&1; then
    echo -e "${GREEN}Restarting phdpd with$PHDPD_FLAGS...${RESET}"
    pkill -x phdpd 2>/dev/null || true
    while pgrep -x phdpd >/dev/null 2>&1; do sleep 0.05; done
fi

if ! pgrep -x phdpd >/dev/null 2>&1; then
    echo -e "${GREEN}Starting phdpd$PHDPD_FLAGS...${RESET}"
    # shellcheck disable=SC2086
    "$PHDPD" $PHDPD_FLAGS &
    PHDPD_PID=$!
    echo -e "${GREEN}phdpd pid=$PHDPD_PID${RESET}"
    for i in $(seq 1 20); do
        [ -S /tmp/phdp.sock ] && break
        # Bail early if the daemon already died — pgrep -x phdpd is
        # too slow for a tight loop, but kill -0 against the captured
        # pid is instant and tells us "process still alive".
        if ! kill -0 "$PHDPD_PID" 2>/dev/null; then
            echo -e "${RED}phdpd (pid=$PHDPD_PID) exited before opening the IPC socket${RESET}"
            echo -e "${RED}  → re-run with DEBUG=v to see why (most likely a UART open failure)${RESET}"
            exit 1
        fi
        sleep 0.1
    done
    if ! pgrep -x phdpd >/dev/null 2>&1; then
        echo -e "${RED}Failed to start phdpd${RESET}"
        exit 1
    fi
fi

# ── Clear pending slots ───────────────────────────────────────────
"$PHDP" clear

# ── Always push os.bin (slot 1) + bank.ofsf (slot 4) alongside ───
# JTAG reset reloads the bitstream but does NOT repopulate data slots;
# whatever was there from the SD-card preset (if the user even selected
# one) is gone. Pushing os.bin + bank.ofsf over UART on every debug run
# decouples the debug path from SD-card state, so the mididemo / test
# suite / duke3d MIDI all see a real SoundFont after reset instead of
# kernel auto-scan returning NULL ("SoundFont NONE" in the app UI).
# Skipped individually when the user is already pushing that exact slot.
OS_BIN="$SDK_DIR/runtime/os.bin"
if [ "$SLOT" != "1" ] && [ -f "$OS_BIN" ]; then
    "$PHDP" push --slot 1 "$OS_BIN"
fi
BANK_OFSF="$SDK_DIR/runtime/bank.ofsf"
if [ "$SLOT" != "4" ] && [ -f "$BANK_OFSF" ]; then
    "$PHDP" push --slot 4 "$BANK_OFSF"
fi

# ── Push file to slot ─────────────────────────────────────────────
"$PHDP" push --slot "$SLOT" "$FILE"

# ── Reset core via JTAG (quartus_pgm reload) ──────────────────────
# Reload the bitstream over JTAG instead of asking phdpd to send a
# soft reset. This guarantees the core comes up in a clean state
# (clears any wedged hardware FSMs from a previous run) before the
# pushed ELF starts executing. The .sof is shipped in runtime/ by
# the OS-side `make sdk DEST=...` target.
#
# TODO: smarter reset later — soft FPGA reset that doesn't reload
# the entire bitstream would be much faster than the ~30s quartus_pgm
# round-trip. For now correctness > speed.
SOF="$SDK_DIR/runtime/ap_core.sof"
if [ ! -f "$SOF" ]; then
    echo -e "${RED}Missing JTAG bitstream: $SOF${RESET}"
    echo "Run 'make sdk DEST=...' from the openfpgaOS source tree to populate runtime/."
    exit 1
fi
if ! command -v quartus_pgm >/dev/null 2>&1; then
    echo -e "${RED}quartus_pgm not on PATH${RESET}"
    echo "Install Quartus or source the Quartus environment first."
    exit 1
fi
echo -e "${GREEN}Resetting core via JTAG (quartus_pgm)...${RESET}"
quartus_pgm -m jtag -o "p;$SOF"

# ── Wait for OS boot ─────────────────────────────────────────────
# After EXEC_START, phdpd writes raw MONITORING bytes directly to its
# own stdout (live byte-for-byte mirror of the Pocket terminal), so
# we don't need to invoke `phdp logs` to stream from the daemon — that
# would just duplicate every byte. Backgrounded phdpd inherits this
# script's stdout/stderr, so the bytes appear inline in the user's
# terminal as the app prints them.
"$PHDP" wait

# Block until the user kills the script (Ctrl+C). phdpd keeps running
# in the background and its stdout keeps flowing to the terminal.
echo -e "${GREEN}Streaming console — Ctrl+C to exit${RESET}"
wait
