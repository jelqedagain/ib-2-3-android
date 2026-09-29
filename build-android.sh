#!/usr/bin/env bash
# Builds the Android (ARM64) version with the NDK in tools/android.
#   ./build-android.sh            -> build-android/ib3android (command-line test build)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
NDK="$(ls -d "$ROOT"/tools/android/android-ndk-* | head -1)"
export PATH="$ROOT/tools/cmake/bin:$ROOT/tools:$PATH"
BUILD="$ROOT/build-android"
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
fi
cmake --build "$BUILD" -- "$@"
