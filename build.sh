#!/bin/sh
# builds vkdeflate into build/ (or $BUILD_DIR); local.sh in the root may set VKDEFLATE_CMAKE_ARGS and VKDEFLATE_BUSY
set -e
cd "$(dirname "$0")"
. tools/busy.sh
BUILD_DIR=${BUILD_DIR:-build}
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ -n "$VKDEFLATE_CMAKE_ARGS" ]; then
    GEN=""
    if command -v ninja >/dev/null 2>&1; then GEN="-G Ninja"; fi
    cmake $GEN -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release $VKDEFLATE_CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
