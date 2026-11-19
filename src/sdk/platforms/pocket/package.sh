#!/bin/bash
#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------
#
# openfpgaOS SDK — Pocket platform packager.
#
# Zips one built Pocket bundle (an APF Cores/Assets/Platforms tree under
# build/pocket/<label>/) into a release ZIP.  Called by the generic
# scripts/package.sh dispatcher — adding a target = add a platform dir
# with its own package.sh of this same shape.
#
# Usage: package.sh <build_dir> <label> <releases_dir>
#   build_dir     a build/pocket/<x>/ directory (APF tree)
#   label         zip basename (the demo core "sdk" is renamed below)
#   releases_dir  where the .zip lands (releases/pocket)
#
set -e
INPUT="$1"; LABEL="$2"; REL="$3"
GREEN='\033[92m'; RESET='\033[0m'
EXCLUDES=("*.DS_Store" "Thumbs.db")

[ -d "$INPUT/Cores" ] || exit 0          # not an APF tree — nothing to do
INPUT=$(cd "$INPUT" && pwd)
mkdir -p "$REL"
REL=$(cd "$REL" && pwd)
[ "$LABEL" = "sdk" ] && LABEL="openfpgaOS-SDK"

CORE_NAME=$(ls "$INPUT/Cores/" 2>/dev/null | head -1)
[ -z "$CORE_NAME" ] && exit 0

GAME_NAME=$(python3 -c '
import json, sys
with open(sys.argv[1]) as source:
    print(json.load(source)["core"]["metadata"]["description"])
' "$INPUT/Cores/$CORE_NAME/core.json" 2>/dev/null || echo "$LABEL")
GAME_VERSION=$(python3 -c '
import json, sys
with open(sys.argv[1]) as source:
    print(json.load(source)["core"]["metadata"]["version"])
' "$INPUT/Cores/$CORE_NAME/core.json" 2>/dev/null || echo "1.0.0")

OUTPUT="$REL/${LABEL}-v${GAME_VERSION}.zip"

cat > "$INPUT/INSTALL.txt" << EOF
$GAME_NAME
$(printf '=%.0s' $(seq 1 ${#GAME_NAME}))

Version: $GAME_VERSION

Installation:
1. Extract this ZIP to your Analogue Pocket SD card root
2. Merge with existing folders if prompted
3. The game will appear in the Pocket menu

Save files are created automatically on first use.
EOF

case "$LABEL" in
    doom|heretic|hexen)
        EXCLUDES+=("*.[wW][aA][dD]")
        cat >> "$INPUT/INSTALL.txt" << EOF

Game WADs are not included. Copy your WAD files to Assets/$LABEL/common/.
EOF
        ;;
esac

# Keep the SDK and bundled libc notices with the distributable ELF without
# adding non-APF directories at the SD-card root. Resolve these from the SDK,
# not the app's top-level license (a game fork may use a different license).
SDK_FILES="$(cd "$(dirname "$0")/../.." && pwd)"
for relative in LICENSE musl/COPYRIGHT; do
    notice="$SDK_FILES/$relative"
    if [ -f "$notice" ]; then
        printf '\n\nBundled component notice: %s\n\n' "$relative" >> "$INPUT/INSTALL.txt"
        cat "$notice" >> "$INPUT/INSTALL.txt"
    fi
done

PACKAGE_TMP=$(mktemp -d "$REL/.package.XXXXXX")
trap 'rm -rf "$PACKAGE_TMP"' EXIT
(cd "$INPUT" && \
 zip -r "$PACKAGE_TMP/bundle.zip" Cores/ Assets/ Platforms/ INSTALL.txt \
     -x "${EXCLUDES[@]}" >/dev/null)
mv "$PACKAGE_TMP/bundle.zip" "$OUTPUT"

echo -e "${GREEN}Package created: $OUTPUT${RESET}"
echo "  Size: $(du -h "$OUTPUT" | cut -f1)"
