#!/bin/sh

#
# Copyright (c) 2026. Yin-Jinlong@github
#

if ! command -v node >/dev/null 2>&1; then
    echo "Error: node not found in PATH" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec node "$SCRIPT_DIR/sync-deps.js" "$@"
