#!/bin/bash
# raw GM20B path (r7-raw, r8-rawca) host build: the mock GPU with nvdec (pack/), frame (raw C++ API), killcheck (a channel
# kill) and the checks and oracles below.
# usage: test/build.sh <tree> <outdir>
#   MetalABI.h: bazel build //drivers/TegraGM20BMetal/abi:header (or METAL_ABI=<its directory>)
set -e -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
METAL=$HERE/..
TREE=$1
OUT=$2
METAL_ABI=${METAL_ABI:-$TREE/bazel-bin/drivers/TegraGM20BMetal/abi}
mkdir -p "$OUT"
# SWITCH_METAL_HARNESS_MARKS: gpu.cpp marks each fetch barrier it leaves out and each draw it splits, for pack/eqcheck.py
FLAGS=(-O2 -g -Wall -Wextra -std=c++17 -DSWITCH_METAL_HARNESS_MARKS -I "$TREE/bazel-bin" -I "$TREE/kits/SwitchGM20BEmit/mesa"
    -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers")
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$METAL/pack/mesa" -c "$METAL/pack/mesa/$f.c" -o "$OUT/$f.o"; done
xcrun clang -O1 -Wall -c -I "$TREE/drivers/TegraGM20B" "$METAL/pack/mock_iokit.c" -o "$OUT/mock_iokit.o"
xcrun clang++ "${FLAGS[@]}" -c -I "$TREE/drivers/TegraGM20B" "$METAL/pack/cap.cpp" -o "$OUT/cap.o"
# the mock's decoder runs the 2D engine's blits too (fx/test/twod_sim: the mip chains)
NVDEC=()
for f in nvdec nvdec_copy nvdec_text nvdec_load nvdec_state nvdec_draw; do
    xcrun clang++ "${FLAGS[@]}" -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" -c "$METAL/pack/$f.cpp" -o "$OUT/$f.o"
    NVDEC+=("$OUT/$f.o")
done
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" -c "$METAL/pack/nvdec_compute.cpp" -o "$OUT/nvdec_compute.o"
xcrun clang++ "${FLAGS[@]}" -c "$METAL/fx/test/twod_sim.cpp" -o "$OUT/twod_sim.o"
# kits/SwitchGM20BEmit's emitters, which gpu.cpp and transfer.cpp call, in the mock library every harness links (their
# generated headers: bazel build //kits/SwitchGM20BEmit:push_headers)
for f in SwitchGM20BEmit3D SwitchGM20BEmitBarrier SwitchGM20BEmitIterated mesa/nouveau_copy_rect; do
    xcrun clang -O2 -std=c11 -DNDEBUG -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" \
        -I "$TREE/third_party/mesa/src/nouveau/headers" -c "$TREE/kits/SwitchGM20BEmit/$f.c" -o "$OUT/kit_${f##*/}.o"
done
xcrun clang++ -dynamiclib "$OUT/mock_iokit.o" "$OUT/cap.o" "${NVDEC[@]}" "$OUT/nvdec_compute.o" "$OUT/twod_sim.o" "$OUT/mme_fermi.o" "$OUT/mme_fermi_sim.o" "$OUT"/kit_*.o -framework IOKit \
    -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$OUT/libmock_iokit.dylib"
OBJC=(-O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -DSWITCH_METAL_HARNESS_MARKS -I "$TREE/bazel-bin" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" -I "$TREE/drivers/TegraGM20BMetal/plugin" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" -I "$TREE/bazel-bin/kits/SwitchGM20B")
# the runtime's completion of states the pack lacks (TegraGM20BMetalComplete.mm, TegraGM20BMetalDirect.mm; completed.cpp beside gpu.cpp)
# the facade's own runtime units: its recording plan (TegraGM20BMetalPlan.h), XR planes, multisample resolves and visibility
FACADE_RUNTIME=("$METAL/semantic_plan.cpp" "$METAL/semantic_resources.cpp" "$METAL/semantic_setters.cpp"
    "$METAL/xr_texture.cpp" "$METAL/multisample.cpp" "$METAL/object_pool.cpp"
    "$METAL/visibility.cpp" "$METAL/visibility_pass.cpp" "$METAL/texture_shape.cpp")
COMPLETION=("$METAL/record_parts.cpp" "$METAL/direct.cpp" "$METAL/direct_compute.cpp" "$METAL/emit.cpp" "$METAL/gm20b_cache.cpp" "$METAL/kernel_cache.cpp" "$METAL/compiler.cpp" "$METAL/metallib.cpp")
# the facade (TegraGM20BMetal.mm and the TegraGM20BMetal*.mm beside it, GM20BAlias.mm, TegraGM20BMetalComplete.mm, TegraGM20BMetalDirect.mm)
FACADE=("$METAL/SwitchSharedEvent.mm")
for f in "$METAL"/TegraGM20BMetal*.mm; do [[ $f == */TegraGM20BMetalPlugin.mm ]] || FACADE+=("$f"); done
LIBS=(-L "$OUT" -lmock_iokit "-Wl,-rpath,$OUT" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/frame.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" \
    "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/frame"
# compchurn: compressed memoryless targets shared across sizes and given back when idle (MOCK_COMPRESSION=1; comp.h CompMem)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/compchurn.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/compchurn"
# lockcheck: begin and commit with no device lock while other threads place programs and make and free textures (build it
# with -fsanitize=thread; MOCK_NATIVE_JOBS=1 MOCK_COMPRESSION=1)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/lockcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/lockcheck"
# geomcheck: the encode's geometry decisions taken both ways, read from the pushes (elided and taken fetch barriers, a Clear
# left out under a covering first draw and kept under a partial one, a split state's prologue launched or not by its pixels)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/geomcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/geomcheck"
# poolcheck: the device's chunks after a burst (gm20b::Device::poolFree, wcFree)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/poolcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/poolcheck"
# arenacheck: MTLHeap arenas (gm20b::Device::createArena, arenaTexture, purgeArenas, restoreArena)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/arenacheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/arenacheck"
# heapframe: MTLHeaps through the facade with no m2v device (TegraGM20BMetalHeap.mm over arena.cpp)
xcrun clang++ "${OBJC[@]}" -Wall -Wextra -I "$METAL" -I "$METAL/pack" -x objective-c++ "$HERE/heapframe.mm" "${FACADE[@]}" \
    -x c++ "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" \
    "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" \
    "$METAL/completed.cpp" "$METAL/compute.cpp" "$METAL/predication.cpp" "$METAL/icb_compute.cpp" "$METAL/compute_loops.cpp" "$METAL/loop_copies.cpp" "$METAL/predicate_loops.cpp" "${COMPLETION[@]}" "${FACADE_RUNTIME[@]}" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none \
    "${LIBS[@]}" -Wl,-undefined,dynamic_lookup -o "$OUT/heapframe"
# killcheck: a channel kill fails exactly the submissions its record names, and the next command buffer gets a new channel
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/killcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/killcheck"
# xrcopycheck: copies between a 550 texture and a buffer move plane 0 alone, each texel's color word (Metal's blits)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/xrcopycheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/xrcopycheck"
# zetacheck: a stored depth/stencil texture's depth and stencil aspects copied with a buffer (NVK's remaps)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/zetacheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/zetacheck"
# indexcheck: an indexed draw past its buffer's indices binds the buffer or the zero page under its limit (caps row 3D-B11)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/indexcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/indexcheck"
# faultcheck: what the device hands the GPU stays valid (build it with -fsanitize=address too)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/faultcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/faultcheck"
# racecheck: what the completion queue adds to the device while the submit thread draws (build it with -fsanitize=thread)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" -I "$TREE/drivers/TegraGM20BMetal/plugin" -I "$METAL_ABI" "$HERE/racecheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" \
    "$METAL/completed.cpp" "$METAL/compute.cpp" "$METAL/predication.cpp" "$METAL/icb_compute.cpp" "$METAL/compute_loops.cpp" "$METAL/loop_copies.cpp" "$METAL/predicate_loops.cpp" "${COMPLETION[@]}" "${FACADE_RUNTIME[@]}" "${LIBS[@]}" -o "$OUT/racecheck"
# footprint: what the device costs a process (its BOs and malloc heap) at open and at its first state, texture and command buffer
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/footprint.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/footprint"
# heapcheck: every state of the pack made in one process, its programs at the VAs its methods name (RAWCAP_LOG=/dev/null)
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/heapcheck.cpp" "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" \
    "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" "$METAL/zeta.cpp" "$METAL/comp.cpp" "${LIBS[@]}" -o "$OUT/heapcheck"
# emitcheck: the per-state registers raw computes from a state's key fields and programs (emit.h) against a pack's records
xcrun clang++ "${FLAGS[@]}" "$HERE/emitcheck.cpp" "$METAL/emit.cpp" "$METAL/pack.cpp" -o "$OUT/emitcheck"
# vertexcheck: emit's vertex input registers against a pack's probed states, each vertex function made with this Mac's Metal
xcrun clang++ -O2 -g -std=c++17 -fobjc-arc -Wall -Wextra -I "$METAL/pack" "$HERE/vertexcheck.mm" "$METAL/emit.cpp" \
    "$METAL/pack.cpp" -framework Metal -framework Foundation -o "$OUT/vertexcheck"
# cachecheck: the runtime's record cache and compile table on a bucket's records, and the AIR hashes on a real library
xcrun clang++ "${FLAGS[@]}" -I "$METAL_ABI" "$HERE/cachecheck.cpp" "$METAL/gm20b_cache.cpp" "$METAL/kernel_cache.cpp" "$METAL/compiler.cpp" "$METAL/metallib.cpp" "$METAL/record.cpp" "$METAL/record_parts.cpp" \
    "$METAL/pack.cpp" -o "$OUT/cachecheck"
xcrun clang++ "${FLAGS[@]}" -I "$TREE/drivers/TegraGM20B" "$HERE/edgecheck.cpp" \
    "$METAL/gpu.cpp" "$METAL/draw_bands.cpp" "$METAL/job_uses.cpp" "$METAL/compute.cpp" "$METAL/predication.cpp" "$METAL/icb_compute.cpp" "$METAL/transfer.cpp" "$METAL/split.cpp" "$METAL/variants.cpp" "$METAL/compute_loops.cpp" "$METAL/loop_copies.cpp" "$METAL/predicate_loops.cpp" \
    "$METAL/fx/fx_mip.cpp" "$METAL/fx/fx_image.cpp" "$METAL/kernel.cpp" "$METAL/kernel_job.cpp" "$METAL/resource_set.cpp" "$METAL/state_segment.cpp" "$METAL/encoder_state.cpp" "$METAL/shared_events.cpp" \
    "$METAL/alias.cpp" "$METAL/arena.cpp" "$METAL/pack.cpp" "$METAL/ycbcr.cpp" \
    "$METAL/zeta.cpp" "$METAL/comp.cpp" "$METAL/completed.cpp" "$METAL/record_parts.cpp" \
    "$METAL/emit.cpp" "${LIBS[@]}" -o "$OUT/edgecheck"
echo "built $OUT/frame $OUT/arenacheck $OUT/heapframe $OUT/compchurn $OUT/lockcheck $OUT/geomcheck $OUT/poolcheck $OUT/killcheck $OUT/xrcopycheck $OUT/zetacheck $OUT/faultcheck $OUT/footprint $OUT/heapcheck $OUT/emitcheck $OUT/vertexcheck $OUT/cachecheck"
echo "native compiler checks: bazel test //drivers/TegraGM20BMetal/test:directcheck_test //tools/gpu/maxwell:direct_consumers_test"
# standinoracle: the frontend's RGB8_444_2P stand-in against this Mac's Metal ($OUT/standinoracle drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.metal)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/standinoracle.mm" -framework Metal -framework Foundation -framework CoreVideo \
    -framework IOSurface -o "$OUT/standinoracle"
echo "built $OUT/standinoracle"
# ycbcrstoreoracle: ycbcr_store.h's rule of a pass's store into two-plane YCbCr attachments, and the raw route of the 8-bit
# code formats (the stand-in, the two-plane store and the 2D engine's halving), against this Mac's Metal
# ($OUT/ycbcrstoreoracle drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.metal)
xcrun clang++ -O1 -std=c++17 -fobjc-arc -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" \
    -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" "$HERE/ycbcrstoreoracle.mm" \
    -x c++ "$METAL/fx/fx_mip.cpp" -framework Metal -framework Foundation -framework CoreVideo -framework IOSurface \
    -o "$OUT/ycbcrstoreoracle"
echo "built $OUT/ycbcrstoreoracle"
# ycbcrblendoracle: blended and masked states into two-plane YCbCr attachments, the stand-in's route against this Mac's Metal
# ($OUT/ycbcrblendoracle pack)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/ycbcrblendoracle.mm" -framework Metal -framework Foundation \
    -framework CoreVideo -framework IOSurface -o "$OUT/ycbcrblendoracle"
echo "built $OUT/ycbcrblendoracle"
# formatoracle, miporacle: passes into and mips of R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float targets, the
# stand-ins' and mip states' route (pack/fmt.metal, a key checkout's) against this Mac's Metal ($OUT/formatoracle pack)
for o in formatoracle miporacle; do
    xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/$o.mm" -framework Metal -framework Foundation -o "$OUT/$o"
    echo "built $OUT/$o"
done
# ycbcrloadoracle: ycbcr_load.h's Load rule and the loaded passes' route against this Mac's Metal
# ($OUT/ycbcrloadoracle pack)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/ycbcrloadoracle.mm" "$METAL/ycbcr.cpp" -framework Metal \
    -framework Foundation -framework CoreVideo -framework IOSurface -o "$OUT/ycbcrloadoracle"
echo "built $OUT/ycbcrloadoracle"
# packedplaneoracle: R10Unorm_PACKED and RG10Unorm_PACKED: the conversion and store states against this Mac's Metal's reads
# ($OUT/packedplaneoracle pack)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/packedplaneoracle.mm" "$METAL/ycbcr.cpp" -framework Metal \
    -framework Foundation -framework CoreVideo -framework IOSurface -o "$OUT/packedplaneoracle"
echo "built $OUT/packedplaneoracle"
# packedreaders: whether Metal, VideoToolbox or Core Image reads a packed 10-bit surface's spare bits ($OUT/packedreaders)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/packedreaders.mm" -framework Metal -framework Foundation -framework CoreVideo \
    -framework IOSurface -framework VideoToolbox -framework CoreImage -framework CoreMedia -o "$OUT/packedreaders"
echo "built $OUT/packedreaders"
# xroracle: the extended-range stand-ins, clears and Core Animation's passes against this Mac's Metal
# ($OUT/xroracle pack/xr.metal <QuartzCore default.metallib>)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/xroracle.mm" -framework Metal -framework Foundation -framework CoreVideo \
    -framework IOSurface -o "$OUT/xroracle"
echo "built $OUT/xroracle"
# resolveoracle: the multisample resolves against this Mac's Metal ($OUT/resolveoracle pack/resolve.metal)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/resolveoracle.mm" -framework Metal -framework Foundation -o "$OUT/resolveoracle"
echo "built $OUT/resolveoracle"
# mip3doracle: the 3D mips a plane a pass against this Mac's Metal ($OUT/mip3doracle pack/mip.metal)
xcrun clang++ -O1 -std=c++17 -fobjc-arc "$HERE/mip3doracle.mm" -framework Metal -framework Foundation -o "$OUT/mip3doracle"
echo "built $OUT/mip3doracle"
xcrun -sdk iphoneos metal -c "$TREE/apps/hwtest/shaders/HWTest.metal" -o "$OUT/HWTest.air"
xcrun -sdk iphoneos metallib "$OUT/HWTest.air" -o "$OUT/HWTest.metallib"
echo "built $OUT/HWTest.metallib"
