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
# usage: build.sh <host libnative_metal_bridge.a> <outdir>
#   the bridge: bazel build //gpu:bridge_darwin (bazel-bin/gpu/bridge_darwin/libnative_metal_bridge.a)
#   then: <outdir>/m2v_gm20b <host libvulkan_nouveau.dylib> <cache dir> [<QuartzCore default.metallib> <pairs>]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$HERE/../../..
mkdir -p "$2"
xcrun clang -O1 -Wall -dynamiclib -I "$TREE/drivers/TegraGM20B" "$HERE/mock_iokit.c" -framework IOKit -framework CoreFoundation \
    -install_name @rpath/libmock_iokit.dylib -o "$2/libmock_iokit.dylib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/m2v_gm20b.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/m2v_gm20b"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/qc_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/qc_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -x objective-c++ \
    "$HERE/present_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" -L "$2" -lmock_iokit \
    -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit -o "$2/present_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$HERE" \
    -x objective-c++ "$HERE/memory_frontend.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" \
    -L "$2" -lmock_iokit -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -o "$2/memory_frontend"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$TREE/drivers/TegraGM20B" \
    -I "$HERE" -x objective-c++ "$HERE/exec_surfaces.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x none "$1" \
    -L "$2" -lmock_iokit -Wl,-rpath,"$2" -lobjc -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
    -o "$2/exec_surfaces"
RAW=$HERE/../raw
GPU=$2/gpu
mkdir -p "$GPU"
for f in mme_fermi mme_fermi_sim; do xcrun clang -O2 -std=c11 -w -I "$RAW/pack/mesa" -c "$RAW/pack/mesa/$f.c" -o "$GPU/$f.o"; done
xcrun clang -O1 -Wall -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/mock_iokit.c" -o "$GPU/mock_iokit.o"
xcrun clang++ -O2 -std=c++17 -c -I "$TREE/drivers/TegraGM20B" "$RAW/pack/cap.cpp" -o "$GPU/cap.o"
xcrun clang++ -O2 -std=c++17 -DNVDEC_TWOD -I "$TREE/drivers/TegraGM20B" -c "$RAW/pack/nvdec.cpp" -o "$GPU/nvdec.o"
xcrun clang++ -O2 -std=c++17 -c "$RAW/fx/test/twod_sim.cpp" -o "$GPU/twod_sim.o"
xcrun clang++ -dynamiclib "$GPU/mock_iokit.o" "$GPU/cap.o" "$GPU/nvdec.o" "$GPU/twod_sim.o" "$GPU/mme_fermi.o" "$GPU/mme_fermi_sim.o" \
    -framework IOKit -framework CoreFoundation -install_name @rpath/libmock_iokit.dylib -o "$GPU/libmock_iokit.dylib"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$TREE/drivers/TegraGM20B" \
    -x objective-c++ "$HERE/alias_frame.mm" "$RAW/runtime/RawMetal.mm" "$RAW/runtime/RawAlias.mm" \
    "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" -x c++ "$RAW/runtime/raw_gpu.cpp" "$RAW/runtime/raw_alias.cpp" \
    "$RAW/fx/fx_mip.cpp" "$RAW/runtime/raw_kernel.cpp" "$RAW/runtime/raw_pack.cpp" "$RAW/runtime/raw_ycbcr.cpp" "$RAW/runtime/raw_zeta.cpp" "$TREE/kits/SwitchRefusal/SwitchRefusal.c" -x none "$1" \
    -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -lobjc -framework Foundation -framework Metal -framework IOSurface \
    -framework IOKit -framework CoreFoundation -o "$GPU/alias_frame"
xcrun clang++ -O1 -g -std=c++17 -Wall -I "$TREE/third_party/mesa/include" -I "$TREE/drivers/TegraGM20B" "$HERE/nvk_alias.cpp" \
    "$RAW/runtime/raw_kernel.cpp" "$RAW/fx/fx_mip.cpp" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -framework IOKit \
    -o "$GPU/nvk_alias"
xcrun clang++ -O1 -g -std=c++17 -Wall -I "$TREE/third_party/mesa/include" -I "$TREE/drivers/TegraGM20B" \
    "$HERE/surface_layout.cpp" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -o "$GPU/surface_layout"
xcrun clang++ -O1 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -I "$TREE/drivers/NativeMetal" -I "$TREE/drivers/TegraGM20B" \
    -x objective-c++ "$HERE/layout_frame.mm" "$TREE/drivers/NativeMetal/NativeBridgeMetalFrontend.mm" \
    -x c++ "$RAW/runtime/raw_kernel.cpp" "$RAW/fx/fx_mip.cpp" -x none "$1" -L "$GPU" -lmock_iokit -Wl,-rpath,"$GPU" -lobjc \
    -framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation \
    -o "$GPU/layout_frame"
echo "built $2/m2v_gm20b $2/qc_frontend $2/present_frontend $2/memory_frontend $GPU/alias_frame $GPU/nvk_alias" \
    "$GPU/surface_layout $GPU/layout_frame"
