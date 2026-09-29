#!/usr/bin/env bash
# Builds the release zip in dist/: the launcher/runtime executable, ANGLE, and the documents.
# The version comes from src/app.rc. The zip name is fixed because the README links to
# releases/latest/download/InfinityBladeIII-PCPort-win64.zip.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="InfinityBladeIII-PCPort-win64"
cd "$ROOT"
./build.sh
STAGE="dist/$NAME"
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE"
cp build/ib3rt.exe "$STAGE/Infinity Blade III.exe"
cp build/libEGL.dll build/libGLESv2.dll "$STAGE/"
cp LICENSE "$STAGE/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$STAGE/THIRD_PARTY_NOTICES.txt"
cp docs/PLAYING.txt "$STAGE/README.txt"
# Strip debug info from the shipped copy (the build keeps it for crash analysis).
tools/llvm-mingw/bin/llvm-strip "$STAGE/Infinity Blade III.exe"
(cd dist && powershell.exe -NoProfile -Command "Compress-Archive -Path '$NAME/*' -DestinationPath '$NAME.zip' -Force")
echo "dist/$NAME.zip"
