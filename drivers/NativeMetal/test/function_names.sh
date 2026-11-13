#!/bin/bash
set -euo pipefail
if [[ $# -ne 6 ]]; then
    echo "usage: $0 <host bridge.a> <local_tools> <MetalABI directory> <fixture.metallib> <names.json> <output>" >&2
    exit 2
fi
BRIDGE=$1
TOOLS=$2
ABI=$3
FIXTURE=$4
NAMES=$5
OUT=$6
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../../.." && pwd)
mkdir -p "$OUT"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -Wno-nullability-completeness -DSWITCH_DEBUG=1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$ABI" -I "$TREE/drivers/NativeMetal" -I "$TREE/drivers/TegraGM20B" \
    "$HERE/function_names.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" "$BRIDGE" \
    -L "$TOOLS/raw" -lmock_iokit "-Wl,-rpath,$TOOLS/raw" \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreVideo \
    -o "$OUT/function_names"
"$OUT/function_names" "$TOOLS/libvulkan_nouveau.dylib" "$OUT/cache" "$FIXTURE" "$NAMES"
