#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
VSCODE_DIR="$ROOT_DIR/vscode"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR"
fi

cmake --build "$BUILD_DIR" --target sc

cd "$VSCODE_DIR"
npm run compile
npx @vscode/vsce package --allow-missing-repository
