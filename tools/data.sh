#!/bin/sh
# the benchmark data: the Silesia corpus, compressed at level 6 with the GDeflate reference encoder (Microsoft's
# DirectStorage repository with NVIDIA's libdeflate fork, cloned into third_party_build); SILESIA may name a folder
# that already holds the twelve files, GDREF_BUILD the encoder's build folder
set -e
cd "$(dirname "$0")/.."
. tools/busy.sh
SIL=${SILESIA:-$PWD/test/silesia}
if [ ! -f "$SIL/dickens" ]; then
    mkdir -p test/silesia
    echo "downloading the silesia corpus, 66 MB"
    curl -L --fail -o test/silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
    unzip -o -q test/silesia.zip -d test/silesia
    rm test/silesia.zip
fi

DS=third_party_build/DirectStorage
if [ ! -f "$DS/GDeflate/3rdparty/libdeflate/libdeflate.h" ]; then
    echo "cloning the gdeflate reference encoder"
    [ -d "$DS/.git" ] || git clone --depth 1 https://github.com/microsoft/DirectStorage "$DS"
    git -C "$DS" submodule update --init --depth 1 GDeflate/3rdparty/libdeflate
fi
GDREF_BUILD=${GDREF_BUILD:-third_party_build/gdref/build}
[ -f "$GDREF_BUILD/CMakeCache.txt" ] || cmake -S tools/gdref -B "$GDREF_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$GDREF_BUILD"

mkdir -p test/vectors
rm -f test/vectors/silesia.txt
for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml; do
    [ -f "test/vectors/$f.l6.gdz" ] || "$GDREF_BUILD/gdref" c "$SIL/$f" "test/vectors/$f.l6.gdz" 6
    echo "test/vectors/$f.l6.gdz" >> test/vectors/silesia.txt
done
