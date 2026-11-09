#!/bin/sh
# bench.sh        builds vkdeflate, fetches and compresses the Silesia corpus, decodes it on the GPU and writes
#                 results/<gpu>.csv
# bench.sh -khr   also builds Khronos's memory decompression layer and decodes every stream with it too
# Close other programs that use the GPU first: kernel times move by 10 % and more when it is shared.
set -e
cd "$(dirname "$0")"
. tools/busy.sh
./build.sh
tools/data.sh
if [ "$1" = "-khr" ]; then
    tools/khr_layer.sh
fi
LAYERS=${LAYER_BUILD:-third_party_build/Vulkan-ExtensionLayer/build}/layers
if [ -f "$LAYERS/VkLayer_khronos_memory_decompression.json" ]; then
    export VK_LAYER_PATH="$(cd "$LAYERS" && pwd)"
fi
"${BUILD_DIR:-build}/bench" -list test/vectors/silesia.txt -csvdir results
