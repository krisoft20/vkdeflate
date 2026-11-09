#!/bin/sh
# builds Khronos's VK_LAYER_KHRONOS_memory_decompression (Vulkan-ExtensionLayer, Apache-2.0 or MIT) into
# third_party_build, for bench; needs git and python: the repository fetches its own dependencies
set -e
cd "$(dirname "$0")/.."
. tools/busy.sh
L=third_party_build/Vulkan-ExtensionLayer
[ -f "$L/CMakeLists.txt" ] || git clone --depth 1 https://github.com/KhronosGroup/Vulkan-ExtensionLayer "$L"
LAYER_BUILD=${LAYER_BUILD:-$L/build}
[ -f "$LAYER_BUILD/CMakeCache.txt" ] || cmake -S "$L" -B "$LAYER_BUILD" -D UPDATE_DEPS=ON -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTS=OFF
cmake --build "$LAYER_BUILD" --target VkLayer_khronos_memory_decompression
