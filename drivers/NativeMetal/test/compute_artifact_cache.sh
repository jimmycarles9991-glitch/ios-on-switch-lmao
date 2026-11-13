#!/bin/bash
set -euo pipefail
if [[ $# -ne 4 ]]; then
    echo "usage: $0 <host bridge.a> <local_tools> <MetalABI directory> <output>" >&2
    exit 2
fi
BRIDGE=$1
TOOLS=$2
ABI=$3
OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../../.." && pwd)
FRONT=$TREE/drivers/NativeMetal
RAW=$TREE/drivers/TegraGM20BMetal/raw/runtime
mkdir -p "$OUT"
xcrun -sdk macosx metal -std=macos-metal2.3 -fno-fast-math -c "$HERE/semantic_ir.metal" -o "$OUT/fixture.air"
xcrun -sdk macosx metal -std=macos-metal2.3 -fno-fast-math -c "$HERE/compute_artifact.metal" -o "$OUT/reflection.air"
xcrun -sdk macosx metallib "$OUT/fixture.air" "$OUT/reflection.air" -o "$OUT/fixture.metallib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_DEBUG=1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$ABI" -I "$FRONT" -I "$TREE/drivers/TegraGM20B" -I "$RAW" \
    "$HERE/compute_artifact_cache.mm" "$RAW/RawDirect.mm" \
    "$RAW/raw_direct.cpp" "$RAW/raw_direct_compute.cpp" "$RAW/raw_record_parts.cpp" "$RAW/raw_emit.cpp" \
    "$RAW/raw_pack.cpp" "$RAW/raw_metallib.cpp" "$RAW/raw_cache.cpp" "$RAW/raw_kernel_cache.cpp" \
    "$BRIDGE" -L "$TOOLS/raw" -lmock_iokit "-Wl,-rpath,$TOOLS/raw" \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreVideo \
    -o "$OUT/compute_artifact_cache"
"$OUT/compute_artifact_cache" "$TOOLS/libvulkan_nouveau.dylib" "$OUT/cache" "$OUT/fixture.metallib"
