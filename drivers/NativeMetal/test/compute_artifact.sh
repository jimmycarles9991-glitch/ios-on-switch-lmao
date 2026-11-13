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
xcrun -sdk macosx metal -std=macos-metal2.3 -c "$HERE/compute_artifact.metal" -o "$OUT/fixture.air"
xcrun -sdk macosx metallib "$OUT/fixture.air" -o "$OUT/fixture.metallib"
FLAGS=(-O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_DEBUG=1)
if [[ -n ${COMPUTE_ARTIFACT_SANITIZER:-} ]]; then
    FLAGS+=("-fsanitize=$COMPUTE_ARTIFACT_SANITIZER" -fno-sanitize-recover=all)
fi
xcrun clang++ "${FLAGS[@]}" -I "$ABI" -I "$FRONT" -I "$TREE/drivers/TegraGM20B" -I "$RAW" \
    "$HERE/compute_artifact.mm" "$HERE/compute_state_fixture.mm" "$RAW/RawMetalComputeState.mm" \
    "$FRONT/NativeBridgeMetalFrontend.mm" "$RAW/RawDirect.mm" \
    "$RAW/raw_direct.cpp" "$RAW/raw_direct_compute.cpp" "$RAW/raw_record_parts.cpp" "$RAW/raw_emit.cpp" \
    "$RAW/raw_pack.cpp" "$RAW/raw_metallib.cpp" "$RAW/raw_cache.cpp" "$RAW/raw_kernel_cache.cpp" \
    "$BRIDGE" -L "$TOOLS/raw" -lmock_iokit "-Wl,-rpath,$TOOLS/raw" \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreVideo \
    -o "$OUT/compute_artifact"
"$OUT/compute_artifact" "$TOOLS/libvulkan_nouveau.dylib" "$OUT/cache" "$OUT/fixture.metallib"
