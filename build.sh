#!/usr/bin/env bash
# Builds ib3rt.exe with the portable LLVM-MinGW toolchain in tools/.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
export PATH="$ROOT/tools/llvm-mingw/bin:$ROOT/tools/cmake/bin:$ROOT/tools:$PATH"
BUILD="$ROOT/build"
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
fi
# A running exe cannot be overwritten, but it can be renamed: move it aside so linking succeeds.
rm -f "$BUILD"/ib3rt.exe.*.old 2>/dev/null || true
if [ -f "$BUILD/ib3rt.exe" ] && ! (: >> "$BUILD/ib3rt.exe") 2>/dev/null; then
    mv -f "$BUILD/ib3rt.exe" "$BUILD/ib3rt.exe.$(date +%s).old"
fi
cmake --build "$BUILD" -- "$@"
