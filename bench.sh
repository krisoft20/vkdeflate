#!/bin/sh
# bench.sh        builds vkdeflate, fetches and compresses the Silesia corpus, decodes it on the GPU and writes
#                 results/<gpu>.csv and results/<gpu>.txt
# bench.sh -khr   also builds Khronos's memory decompression layer and decodes every stream with it too
# Close other programs that use the GPU first: kernel times move by 10 % and more when it is shared.
# The corpus is downloaded to test/silesia (66 MB); SILESIA may name a folder that already holds the twelve files.
# The GDeflate reference encoder (Microsoft's DirectStorage repository with NVIDIA's libdeflate fork) and the
# Khronos layer (Vulkan-ExtensionLayer, which needs python) are cloned and built in third_party_build, at the
# commits in tools/third_party.sh;
# GDREF_BUILD and LAYER_BUILD may name other build folders. In a release package, which has no CMakeLists.txt,
# bench and the encoder come built.
set -e
cd "$(dirname "$0")"
. tools/busy.sh
GDREF_BUILD=${GDREF_BUILD:-third_party_build/gdref/build}
if [ -f CMakeLists.txt ]; then
    ./build.sh
fi

SIL=${SILESIA:-$PWD/test/silesia}
if [ ! -f "$SIL/dickens" ]; then
    mkdir -p test/silesia
    echo "downloading the silesia corpus, 66 MB"
    curl -L --fail -o test/silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
    unzip -o -q test/silesia.zip -d test/silesia
    rm test/silesia.zip
fi

DS=third_party_build/DirectStorage
if [ -f CMakeLists.txt ]; then
    if [ ! -f "$DS/GDeflate/3rdparty/libdeflate/libdeflate.h" ]; then
        echo "cloning the gdeflate reference encoder"
        sh tools/fetch.sh directstorage
    fi
    [ -f "$GDREF_BUILD/CMakeCache.txt" ] || cmake -S tools/gdref -B "$GDREF_BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$GDREF_BUILD"
fi

mkdir -p test/vectors
rm -f test/vectors/silesia.txt
for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml; do
    [ -f "test/vectors/$f.l6.gdz" ] || "$GDREF_BUILD/gdref" c "$SIL/$f" "test/vectors/$f.l6.gdz" 6
    echo "test/vectors/$f.l6.gdz" >> test/vectors/silesia.txt
done

L=third_party_build/Vulkan-ExtensionLayer
LAYER_BUILD=${LAYER_BUILD:-$L/build}
if [ "$1" = "-khr" ]; then
    [ -f "$L/CMakeLists.txt" ] || sh tools/fetch.sh layer
    [ -f "$LAYER_BUILD/CMakeCache.txt" ] || cmake -S "$L" -B "$LAYER_BUILD" -D UPDATE_DEPS=ON -D CMAKE_BUILD_TYPE=Release -D BUILD_TESTS=OFF
    cmake --build "$LAYER_BUILD" --target VkLayer_khronos_memory_decompression
fi
if [ -f "$LAYER_BUILD/layers/VkLayer_khronos_memory_decompression.json" ]; then
    export VK_LAYER_PATH="$(cd "$LAYER_BUILD/layers" && pwd)"
fi
"${BUILD_DIR:-build}/bench" -list test/vectors/silesia.txt -csvdir results
