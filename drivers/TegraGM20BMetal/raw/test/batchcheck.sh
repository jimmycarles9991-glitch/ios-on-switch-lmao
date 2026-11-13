#!/bin/bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "usage: $0 <output>" >&2
    exit 2
fi
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
RAW=$(cd "$HERE/.." && pwd)
TREE=$(cd "$HERE/../../../.." && pwd)
# the frontend's MetalABI.h: bazel build //drivers/NativeMetal/abi:header (or METAL_ABI=<its directory>)
METAL_ABI=${METAL_ABI:-$TREE/bazel-bin/drivers/NativeMetal/abi}
# the push headers (bazel build //kits/SwitchGM20BEmit:push_headers, or PUSH_HEADERS=<their directory>)
PUSH_HEADERS=${PUSH_HEADERS:-$TREE/bazel-bin/kits/SwitchGM20BEmit/gen}
FLAGS=(-O1 -g -Wall -Wextra -std=c++17 -DSWITCH_RAW_HARNESS_MARKS -fsanitize=address,undefined -fno-sanitize-recover=all
    -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$PUSH_HEADERS" -I "$TREE/third_party/mesa/src/nouveau/headers")
for source in mme_fermi mme_fermi_sim; do
    xcrun clang -O1 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$source.c" -o "$OUT/$source.o"
done
xcrun clang -O1 -g -Wall -Wextra -fsanitize=address,undefined -I "$TREE/drivers/TegraGM20B" \
    -DSWITCH_MOCK_JOB_INTERFACE -c "$RAW/pack/mock_iokit.c" -o "$OUT/mock_iokit.o"
for source in cap nvdec nvdec_compute; do
    xcrun clang++ "${FLAGS[@]}" -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" \
        -c "$RAW/pack/$source.cpp" -o "$OUT/$source.o"
done
xcrun clang++ "${FLAGS[@]}" -c "$RAW/fx/test/twod_sim.cpp" -o "$OUT/twod_sim.o"
# kits/SwitchGM20BEmit's emitters, which raw_gpu.cpp and raw_transfer.cpp call, in the mock library every harness links (their
# generated headers: bazel build //kits/SwitchGM20BEmit:push_headers)
for f in SwitchGM20BEmit3D SwitchGM20BEmitBarrier SwitchGM20BEmitIterated mesa/nouveau_copy_rect; do
    xcrun clang -O2 -std=c11 -DNDEBUG -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$PUSH_HEADERS" \
        -I "$TREE/third_party/mesa/src/nouveau/headers" -c "$TREE/kits/SwitchGM20BEmit/$f.c" -o "$OUT/kit_${f##*/}.o"
done
xcrun clang++ -dynamiclib -fsanitize=address,undefined "$OUT/mock_iokit.o" "$OUT/cap.o" "$OUT/nvdec.o" \
    "$OUT/nvdec_compute.o" "$OUT/twod_sim.o" "$OUT/mme_fermi.o" "$OUT/mme_fermi_sim.o" "$OUT"/kit_*.o \
    -framework IOKit -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
SOURCES=()
for source in gpu transfer split kernel shared_events alias arena pack ycbcr zeta comp completed compute \
    record_parts direct direct_compute emit cache kernel_cache compiler metallib state_segment resource_set variants kernel_job encoder_state job_uses multisample; do
    SOURCES+=("$RAW/runtime/raw_$source.cpp")
done
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" \
    "$HERE/batchcheck.cpp" "${SOURCES[@]}" "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" \
    -L "$OUT" -lmock_iokit "-Wl,-rpath,$OUT" -lobjc -framework Foundation -framework Metal \
    -framework IOSurface -framework IOKit -framework CoreFoundation -o "$OUT/batchcheck"
# stagecheck: a pack state's set 0 against what each stage and slot binds (stagecheck.py runs its programs over it)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" \
    "$HERE/stagecheck.cpp" "${SOURCES[@]}" "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" \
    -L "$OUT" -lmock_iokit "-Wl,-rpath,$OUT" -lobjc -framework Foundation -framework Metal \
    -framework IOSurface -framework IOKit -framework CoreFoundation -o "$OUT/stagecheck"
for mode in legacy native; do
    if [[ $mode == native ]]; then export MOCK_NATIVE_JOBS=1; else unset MOCK_NATIVE_JOBS; fi
    for delay in 0 100; do
        MOCK_GPU_US=$delay "$OUT/batchcheck" >"$OUT/$mode-$delay.log" 2>&1
    done
done
