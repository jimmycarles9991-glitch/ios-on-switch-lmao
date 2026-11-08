#!/bin/bash
# raw GM20B path (r7-raw, r8-rawca) host build: the mock GPU with nvdec (pack/), nvkframe (frontend + host bridge + NVK twin),
# rawframe (raw C++ API), killcheck (a channel kill), facadeframe (RawMetal.mm over the frontend as its m2v fallback), blitframe (copies), orderframe (the
# two drivers' order on the GPU, over the mock's asynchronous GPU) and cabench (CPU per frame).
# usage: test/build.sh <host libnative_metal_bridge.a> <frontend dir (NativeBridgeMetalFrontend.{mm,h})> <tree> <outdir>
#   the frontend's MetalABI.h: bazel build //drivers/NativeMetal/abi:header (or METAL_ABI=<its directory>)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
RAW=$HERE/..
BRIDGE=$1
FRONT=$2
TREE=$3
OUT=$4
METAL_ABI=${METAL_ABI:-$TREE/bazel-bin/drivers/NativeMetal/abi}
mkdir -p "$OUT"
# SWITCH_RAW_HARNESS_MARKS: raw_gpu.cpp marks each fetch barrier it leaves out and each draw it splits, for pack/eqcheck.py
FLAGS=(-O2 -g -Wall -Wextra -std=c++17 -DSWITCH_RAW_HARNESS_MARKS)
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$f.c" -o "$OUT/$f.o"; done
xcrun clang -O1 -Wall -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/mock_iokit.c" -o "$OUT/mock_iokit.o"
xcrun clang++ "${FLAGS[@]}" -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/cap.cpp" -o "$OUT/cap.o"
# the mock's decoder runs the 2D engine's blits too (raw/fx/test/twod_sim: the mip chains)
xcrun clang++ "${FLAGS[@]}" -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" -c "$RAW/pack/nvdec.cpp" -o "$OUT/nvdec.o"
xcrun clang++ "${FLAGS[@]}" -c "$RAW/fx/test/twod_sim.cpp" -o "$OUT/twod_sim.o"
xcrun clang++ -dynamiclib "$OUT/mock_iokit.o" "$OUT/cap.o" "$OUT/nvdec.o" "$OUT/twod_sim.o" "$OUT/mme_fermi.o" "$OUT/mme_fermi_sim.o" -framework IOKit \
    -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
OBJC=(-O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_RAW_HARNESS_MARKS -I "$FRONT" -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B")
# the runtime's completion of states the pack lacks (RawComplete.mm, RawProbe.mm; raw_completed.cpp beside raw_gpu.cpp); nvdec is
# the mock's
COMPLETION=("$RAW/runtime/raw_capture.cpp" "$RAW/runtime/raw_record.cpp" "$RAW/runtime/raw_cache.cpp" "$RAW/runtime/raw_compiler.cpp" "$RAW/runtime/raw_metallib.cpp")
# the raw facade (RawMetal.mm and the RawMetal*.mm beside it, RawAlias.mm, RawComplete.mm, RawProbe.mm)
FACADE=("$RAW"/runtime/Raw*.mm)
LIBS=(-L "$OUT" -lmock_iokit "-Wl,-rpath,$OUT" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation)
xcrun clang++ "${OBJC[@]}" -x objective-c++ "$HERE/nvkframe.mm" "$FRONT/NativeBridgeMetalFrontend.mm" -x none "$BRIDGE" "${LIBS[@]}" -o "$OUT/nvkframe" 2>&1 | grep -v "duplicate libraries" || true
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/rawframe.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" \
    "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/rawframe"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -DRAW_FACADE -x objective-c++ "$HERE/nvkframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/facadeframe" 2>&1 | grep -v "duplicate libraries" || true
# poolcheck: the raw device's chunks after a burst (raw::Device::poolFree, wcFree)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/poolcheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/poolcheck"
# killcheck: a channel kill fails exactly the submissions its record names, and the next command buffer gets a new channel
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/killcheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/killcheck"
# faultcheck: what the raw device hands the GPU stays valid (build it with -fsanitize=address too)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/faultcheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/faultcheck"
# racecheck: what the completion queue adds to the device while the submit thread draws (build it with -fsanitize=thread)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/racecheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" \
    "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "${LIBS[@]}" -o "$OUT/racecheck"
# footprint: what the raw device costs a process (its BOs and malloc heap) at open and at its first state, texture and command buffer
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/footprint.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/footprint"
# heapcheck: every state of the pack made in one process, its programs at the VAs its methods name (RAWCAP_LOG=/dev/null)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/heapcheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/heapcheck"
# datacheck: the pack's states whose programs read NAK's shader data, drawn raw (test/datacheck.py holds them against the probe)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/datacheck.cpp" "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "${LIBS[@]}" -o "$OUT/datacheck"
# recordcheck: the runtime's records of states (raw_record.cpp) against the pack mkpack.py made from the same probe runs
xcrun clang++ "${FLAGS[@]}" "$HERE/recordcheck.cpp" "$RAW/runtime/raw_record.cpp" "$RAW/runtime/raw_pack.cpp" -o "$OUT/recordcheck"
# emitcheck: the per-state registers raw computes from a state's key fields and programs (raw_emit.h) against a pack's records
xcrun clang++ "${FLAGS[@]}" "$HERE/emitcheck.cpp" "$RAW/runtime/raw_emit.cpp" "$RAW/runtime/raw_pack.cpp" -o "$OUT/emitcheck"
# vertexcheck: raw_emit's vertex input registers against a pack's probed states, each vertex function made with this Mac's Metal
xcrun clang++ -O2 -g -std=c++17 -fobjc-arc -Wall -Wextra -I "$RAW/pack" "$HERE/vertexcheck.mm" "$RAW/runtime/raw_emit.cpp" \
    "$RAW/runtime/raw_pack.cpp" -framework Metal -framework Foundation -o "$OUT/vertexcheck"
# invocationcheck: a call recorded as an invocation keeps what its pointer arguments pointed to (raw_invocation.h)
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wall -Wextra "$HERE/invocationcheck.mm" -framework Foundation -framework Metal \
    -o "$OUT/invocationcheck"
# cachecheck: the runtime's record cache and compile table on a bucket's records, and the AIR hashes on a real library
xcrun clang++ "${FLAGS[@]}" "$HERE/cachecheck.cpp" "$RAW/runtime/raw_cache.cpp" "$RAW/runtime/raw_compiler.cpp" "$RAW/runtime/raw_metallib.cpp" "$RAW/runtime/raw_record.cpp" \
    "$RAW/runtime/raw_pack.cpp" -o "$OUT/cachecheck"
# completecheck: the runtime's own probe of every Core Animation state of a corpus, its records against the pack
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/completecheck.mm" "$RAW/runtime/RawProbe.mm" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_pack.cpp" "${COMPLETION[@]}" -x none "$BRIDGE" "${LIBS[@]}" -o "$OUT/completecheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/nvkframe $OUT/rawframe $OUT/facadeframe $OUT/poolcheck $OUT/killcheck $OUT/faultcheck $OUT/footprint $OUT/heapcheck $OUT/datacheck $OUT/recordcheck $OUT/emitcheck $OUT/vertexcheck $OUT/cachecheck $OUT/completecheck"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/blitframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/blitframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/blitframe"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/orderframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/orderframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/orderframe"
# splitframe: command buffers in segments (raw, m2v, raw) from several threads: chains intact, every scheduled handler before its
# completed ones (MOCK_GPU_US=0 races the completion thread against the submitting one), order on the GPU (ordercheck.py)
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/splitframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/splitframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/splitframe"
# surfaceorder: an IOSurface one process's raw path writes, read by another process's raw path or m2v after the write on the GPU
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/surfaceorder.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/surfaceorder" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/surfaceorder"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/selcheck.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/selcheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/selcheck"
# contractcheck: Metal contracts the facade keeps where the raw path cannot draw (build it with -fsanitize=address too)
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/contractcheck.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/contractcheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/contractcheck"
# schedcheck: scheduled handlers run at the fixed priority of the thread that commits and waits for them
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/schedcheck.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/schedcheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/schedcheck"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/planarframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/planarframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/planarframe"
xcrun clang++ "${OBJC[@]}" -x objective-c++ "$HERE/cabench.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/cabench" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/cabench"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/mipframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/mipframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/mipframe"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/levelframe.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/levelframe" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/levelframe"
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -x objective-c++ "$HERE/shapecheck.mm" "${FACADE[@]}" "$FRONT/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_completed.cpp" "${COMPLETION[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$BRIDGE" "${LIBS[@]}" \
    -o "$OUT/shapecheck" 2>&1 | grep -v "duplicate libraries" || true
echo "built $OUT/shapecheck"
# standinoracle: the frontend's RGB8_444_2P stand-in against this Mac's Metal ($OUT/standinoracle drivers/NativeMetal/TwoPlaneStandIn.metal)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/standinoracle.mm" -framework Metal -framework Foundation -framework CoreVideo \
    -framework IOSurface -o "$OUT/standinoracle"
echo "built $OUT/standinoracle"
# xroracle: the extended-range stand-ins, clears and Core Animation's passes against this Mac's Metal
# ($OUT/xroracle pack/xr.metal <QuartzCore default.metallib>)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/xroracle.mm" -framework Metal -framework Foundation -framework CoreVideo \
    -framework IOSurface -o "$OUT/xroracle"
echo "built $OUT/xroracle"
