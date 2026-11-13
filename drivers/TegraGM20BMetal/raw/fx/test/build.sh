#!/bin/bash
# raw/fx host proof (r12-cafx): builds the NIL oracle (over the NVK host twin and the hostsim mock GM20B) and mipcheck, runs both.
# usage: test/build.sh <host libvulkan_nouveau.dylib> <Mesa source tree (its include/ has vulkan/vulkan.h)> <outdir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FX=$HERE/..
TREE=$FX/../../../..
NVK=$1
MESA=$2
OUT=$3
mkdir -p "$OUT"
xcrun clang -O1 -Wall -dynamiclib -I "$TREE/drivers/TegraGM20B" "$TREE/drivers/TegraGM20BMetal/hostsim/mock_iokit.c" \
    -framework IOKit -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
xcrun clang -O1 -Wall -I "$MESA/include" "$HERE/niloracle.c" -L "$OUT" -lmock_iokit -Wl,-rpath,"$OUT" -o "$OUT/niloracle"
xcrun clang++ -O2 -g -Wall -Wextra -std=c++17 -I "$TREE/third_party/mesa/src/nouveau/headers" "$HERE/mipcheck.cpp" "$HERE/twod_sim.cpp" "$FX/fx_mip.cpp" -o "$OUT/mipcheck"
SIZES=(1x1x1 2x2x2 2x1x2 3x5x3 776x24x10 776x24x3 180x180x8 180x180x1 120x120x7 60x60x6 40x40x6 1280x720x11 1280x720x1
    720x1280x11 1024x1x11 1x1024x11 4096x4096x13 2048x1536x12 97x3x7 33x65x7 512x512x10 1366x1024x11 256x8x9 8x256x9
    16384x16384x15 300x200x9 1x300x9 640x480x10 11x11x4 22x22x5 43x43x6 86x86x7 85x85x7 42x42x6 21x21x5 10x10x4 17x9x5)
"$OUT/niloracle" "$NVK" 44 "${SIZES[@]}" >"$OUT/nil-bgra8.txt" 2>"$OUT/niloracle.log"
"$OUT/niloracle" "$NVK" 97 "${SIZES[@]}" >"$OUT/nil-rgba16f.txt" 2>>"$OUT/niloracle.log"
"$OUT/mipcheck" "$OUT/nil-bgra8.txt" "$OUT/nil-rgba16f.txt"
