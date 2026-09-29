#!/usr/bin/env bash
# Fetches the dependencies that are too large to keep in the repository:
#   external/dynarmic       ARM64 -> x86-64 JIT (git submodule)
#   external/boost-1.92.0   Boost headers (dynarmic needs them)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

git submodule update --init --recursive

BOOST_VERSION=1.92.0
BOOST_DIR="external/boost-$BOOST_VERSION"
if [ ! -d "$BOOST_DIR/boost" ]; then
    archive="boost_${BOOST_VERSION//./_}.tar.gz"
    echo "Downloading Boost $BOOST_VERSION..."
    curl -L --fail -o "/tmp/$archive" "https://archives.boost.io/release/$BOOST_VERSION/source/$archive"
    mkdir -p "$BOOST_DIR"
    tar -xzf "/tmp/$archive" -C "$BOOST_DIR" --strip-components=1 "boost_${BOOST_VERSION//./_}/boost" \
        "boost_${BOOST_VERSION//./_}/LICENSE_1_0.txt"
    rm -f "/tmp/$archive"
fi
echo "Dependencies are ready."
