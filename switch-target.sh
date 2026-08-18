#!/usr/bin/env bash
set -euo pipefail

TARGET="esp32s3"
if [[ $# -gt 0 ]]; then
    echo "Usage: $0" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDKCONFIG_DEST="$SCRIPT_DIR/sdkconfig"
SDKCONFIG_SRC="$SCRIPT_DIR/sdkconfig.esp32s3"

if [[ ! -f "$SDKCONFIG_SRC" ]]; then
    echo "Error: $SDKCONFIG_SRC not found" >&2
    exit 1
fi

CURRENT_TARGET=""
if [[ -f "$SDKCONFIG_DEST" ]]; then
    CURRENT_TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"$/\1/p' "$SDKCONFIG_DEST")"
fi

if [[ "$CURRENT_TARGET" != "$TARGET" ]]; then
    echo "Switching from '${CURRENT_TARGET:-none}' to '$TARGET'"
    idf.py set-target "$TARGET"
    cp "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"
    idf.py fullclean
else
    echo "Already targeting $TARGET"
    if ! cmp -s "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"; then
        echo "Restoring $SDKCONFIG_SRC"
        cp "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"
        idf.py fullclean
    fi
fi

echo "Target: $TARGET"
echo "Run: idf.py build"
