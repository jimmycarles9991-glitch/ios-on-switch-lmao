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
xcrun -sdk macosx metallib "$OUT/fixture.air" -o "$OUT/fixture.metallib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_DEBUG=1 \
    -I "$ABI" -I "$FRONT" -I "$TREE/drivers/TegraGM20B" -I "$RAW" \
    "$HERE/semantic_ir.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
    "$RAW/raw_direct.cpp" "$RAW/raw_record_parts.cpp" "$RAW/raw_emit.cpp" "$RAW/raw_pack.cpp" "$RAW/raw_metallib.cpp" \
    "$BRIDGE" -L "$TOOLS/raw" -lmock_iokit "-Wl,-rpath,$TOOLS/raw" \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreVideo \
    -o "$OUT/semantic_ir"
"$OUT/semantic_ir" "$TOOLS/libvulkan_nouveau.dylib" "$OUT/cache" "$OUT/fixture.metallib" "$OUT"
if [[ -z ${SWITCH_IR_CASE:-} ]]; then
    for CASE in vector private extended unpack_half convert texture; do
        SWITCH_IR_CASE="semantic_$CASE" NAK_DEBUG=print "$OUT/semantic_ir" \
            "$TOOLS/libvulkan_nouveau.dylib" "$OUT/cache" "$OUT/fixture.metallib" "$OUT" \
            >"$OUT/$CASE-nir.txt" 2>&1
    done
    python3 "$HERE/semantic_execution.py" "$OUT"
fi
