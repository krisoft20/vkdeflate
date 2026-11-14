#!/bin/sh
# tools/fetch.sh directstorage|layer   clones Microsoft's DirectStorage (with NVIDIA's libdeflate) or Khronos's
# Vulkan-ExtensionLayer into third_party_build at the commit named in tools/third_party.sh
set -e
cd "$(dirname "$0")/.."
. tools/third_party.sh
case "$1" in
directstorage) dir=third_party_build/DirectStorage; url=https://github.com/microsoft/DirectStorage; ref=$DIRECTSTORAGE ;;
layer) dir=third_party_build/Vulkan-ExtensionLayer; url=https://github.com/KhronosGroup/Vulkan-ExtensionLayer; ref=$EXTENSION_LAYER ;;
*) echo "usage: tools/fetch.sh directstorage|layer"; exit 1 ;;
esac
if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
fi
git -C "$dir" fetch -q --depth 1 origin "$ref"
git -C "$dir" checkout -q FETCH_HEAD
if [ "$1" = directstorage ]; then
    git -C "$dir" submodule update --init --depth 1 GDeflate/3rdparty/libdeflate
fi
