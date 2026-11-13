#!/bin/sh
# Host simulation of the stage-4 stack: NativeBridgeMetalFrontend.mm + the metal2vulkan bridge + NVK (macOS host build,
# nvkmd_darwin) over mock_iokit.c, a GM20B user client that completes every submission at once. It proves device creation,
# metal2vulkan's acceptance of NVK's feature set and NAK's SM53 compile of QuartzCore's shaders; nothing executes on a GPU.
# qc_frontend.mm: QuartzCore's pipelines through the frontend (sheng's 24 recipes, the framebuffer-fetch filters, the uber
# shader under 33 constant sets), each made and drawn three times in one pass: <outdir>/qc_frontend <nvk> <cache> <QC metallib>.
# present_frontend.mm: the command buffer's presentDrawable: contract (Metal's): <outdir>/present_frontend <nvk> <cache>.
# memory_frontend.mm: what a client's GPU memory costs it as iOS charges it (iokit_mapped), memoryless textures sharing a
# backing, and the idle blocks going back: <outdir>/memory_frontend <nvk> <cache> [<width>x<height> ...] (default: the widgets of
# WidgetRenderer's 23:44 memory warning).
# exec_surfaces.mm: each submission names the host imports it uses (ExecSurfaces), written or read: <outdir>/exec_surfaces <nvk>
# <cache>.
# alias_frame.mm: m2v's views of the raw path's memory are that memory, and an IOSurface is imported once, over raw/pack's mock
# GPU instead (its copy engine runs through the mock VM): <outdir>/gpu/alias_frame <cache> <nvk>.
# nvk_alias.cpp: a raw texture's span and a raw mip chain are NVK images in place, at the raw path's VA and in its layout:
# <outdir>/gpu/nvk_alias <nvk>.
# surface_layout.cpp: block-linear IOSurfaces through NVK (the mock decides layouts as the kernel does): a writer-first surface is
# an optimal image at its import's VA, another process reads the same texels, a sampler-first one stays pitch, and NIL's layout
# is TegraGM20BLayout.h's over a sweep: <outdir>/gpu/surface_layout <nvk>.
# layout_frame.mm: the frontend's optimal views (a raw mip chain, a writer-first surface) and refusals: <outdir>/gpu/layout_frame
# <cache> <nvk>.
# fault_frame.mm: a GPU fault fails the command buffers it takes and the queue and device run on (the mock's channel kill):
# <outdir>/gpu/fault_frame <cache> <nvk>.
# usage: build.sh <host libnative_metal_bridge.a> <outdir>
#   the bridge: bazel build //gpu:bridge_darwin (bazel-bin/gpu/bridge_darwin/libnative_metal_bridge.a); the frontend's
#   MetalABI.h: bazel build //drivers/NativeMetal/abi:header (or METAL_ABI=<its directory>)
#   then: <outdir>/m2v_gm20b <host libvulkan_nouveau.dylib> <cache dir> [<QuartzCore default.metallib> <pairs>]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$HERE/../../..
METAL_ABI=${METAL_ABI:-$TREE/bazel-bin/drivers/NativeMetal/abi}
mkdir -p "$2"
xcrun clang -O1 -Wall -dynamiclib -I "$TREE/drivers/TegraGM20B" "$HERE/mock_iokit.c" -framework IOKit -framework CoreFoundation \
    -install_name @rpath/libmock_iokit.dylib -o "$2/libmock_iokit.dylib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -x objective-c++ \
    "$HERE/m2v_gm20b.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/m2v_gm20b"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -x objective-c++ \
    "$HERE/qc_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/qc_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -x objective-c++ \
    "$HERE/present_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/present_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$HERE" \
    -x objective-c++ "$HERE/memory_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" \
    -L "$2" -lmock_iokit -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -o "$2/memory_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" \
    -I "$HERE" -x objective-c++ "$HERE/exec_surfaces.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" \
    -L "$2" -lmock_iokit -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -o "$2/exec_surfaces"
RAW=$HERE/../raw
GPU=$2/gpu
mkdir -p "$GPU"
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$f.c" -o "$GPU/$f.o"; done
xcrun clang -O1 -Wall -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/mock_iokit.c" -o "$GPU/mock_iokit.o"
xcrun clang++ -O2 -std=c++17 -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/cap.cpp" -o "$GPU/cap.o"
xcrun clang++ -O2 -std=c++17 -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" -I "$TREE/third_party/mesa/src/nouveau/headers" \
    -c "$RAW/pack/nvdec.cpp" -o "$GPU/nvdec.o"
xcrun clang++ -O2 -std=c++17 -I "$TREE/drivers/TegraGM20B" -c "$RAW/pack/nvdec_compute.cpp" -o "$GPU/nvdec_compute.o"
xcrun clang++ -O2 -std=c++17 -c "$RAW/fx/test/twod_sim.cpp" -o "$GPU/twod_sim.o"
xcrun clang++ -O2 -std=c++17 -c "$RAW/runtime/raw_capture.cpp" -o "$GPU/raw_capture.o"
xcrun clang++ -O2 -std=c++17 -c "$RAW/runtime/raw_pack.cpp" -o "$GPU/raw_pack_mock.o"
# kits/SwitchGM20BEmit's emitters, which raw_gpu.cpp and raw_transfer.cpp call, in the mock library every harness links (their
# generated headers: bazel build //kits/SwitchGM20BEmit:push_headers)
for f in SwitchGM20BEmit3D SwitchGM20BEmitBarrier SwitchGM20BEmitIterated mesa/nouveau_copy_rect; do
    xcrun clang -O2 -std=c11 -DNDEBUG -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" \
        -I "$TREE/third_party/mesa/src/nouveau/headers" -c "$TREE/kits/SwitchGM20BEmit/$f.c" -o "$GPU/kit_${f##*/}.o"
done
xcrun clang++ -dynamiclib "$GPU/mock_iokit.o" "$GPU/cap.o" "$GPU/nvdec.o" "$GPU/nvdec_compute.o" "$GPU/twod_sim.o" "$GPU/raw_capture.o" "$GPU/raw_pack_mock.o" \
    "$GPU/mme_fermi.o" "$GPU/mme_fermi_sim.o" "$GPU"/kit_*.o \
    -framework IOKit -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$GPU/libmock_iokit.dylib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" \
    -x objective-c++ "$HERE/alias_frame.mm" "$RAW"/runtime/Raw*.mm "$RAW/runtime/SwitchSharedEvent.mm" \
    "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/runtime/raw_draw_bands.cpp" "$RAW/runtime/raw_split.cpp" "$RAW/runtime/raw_variants.cpp" "$RAW/runtime/raw_alias.cpp" "$RAW/runtime/raw_arena.cpp" \
    "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_kernel_job.cpp" "$RAW/runtime/raw_resource_set.cpp" "$RAW/runtime/raw_state_segment.cpp" "$RAW/runtime/raw_encoder_state.cpp" "$RAW/runtime/raw_shared_events.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$RAW/runtime/raw_comp.cpp" "$RAW/runtime/raw_capture.cpp" "$RAW/runtime/raw_record.cpp" \
    "$RAW/runtime/raw_cache.cpp" "$RAW/runtime/raw_compiler.cpp" "$RAW/runtime/raw_metallib.cpp" "$RAW/runtime/raw_completed.cpp" "$RAW/runtime/raw_compute.cpp" "$RAW/runtime/raw_compute_loops.cpp" "$RAW/runtime/raw_loop_copies.cpp" "$RAW/runtime/raw_predication.cpp" "$RAW/runtime/raw_predicate_loops.cpp" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$1" \
    -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -lobjc -framework Foundation -framework Metal -framework IOSurface \
    -framework IOKit -framework CoreFoundation -o "$GPU/alias_frame"
xcrun clang++ -O1 -g -std=c++17 -Wall -I "$TREE/third_party/mesa/include" -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" "$HERE/nvk_alias.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_kernel_job.cpp" "$RAW/runtime/raw_resource_set.cpp" "$RAW/runtime/raw_state_segment.cpp" "$RAW/runtime/raw_encoder_state.cpp" "$RAW/runtime/raw_shared_events.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -framework IOKit \
    -o "$GPU/nvk_alias"
xcrun clang++ -O1 -g -std=c++17 -Wall -I "$TREE/third_party/mesa/include" -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" \
    "$HERE/surface_layout.cpp" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -o "$GPU/surface_layout"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" \
    -x objective-c++ "$HERE/layout_frame.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_kernel_job.cpp" "$RAW/runtime/raw_resource_set.cpp" "$RAW/runtime/raw_state_segment.cpp" "$RAW/runtime/raw_encoder_state.cpp" "$RAW/runtime/raw_shared_events.cpp" "$RAW/fx/fx_mip.cpp" "$RAW/fx/fx_image.cpp" -x none "$1" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -lobjc \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation \
    -o "$GPU/layout_frame"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$METAL_ABI" -I "$TREE/drivers/TegraGM20B" -I "$TREE/kits/SwitchGM20BEmit/mesa" -I "$TREE/bazel-bin/kits/SwitchGM20BEmit/gen" -I "$TREE/third_party/mesa/src/nouveau/headers" \
    -x objective-c++ "$HERE/fault_frame.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$GPU" \
    -lmock_iokit -Wl,-rpath,"$GPU" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -framework CoreFoundation -o "$GPU/fault_frame"
echo "built $2/m2v_gm20b $2/qc_frontend $2/present_frontend $2/memory_frontend $GPU/alias_frame $GPU/nvk_alias" \
    "$GPU/surface_layout $GPU/layout_frame $GPU/fault_frame"
