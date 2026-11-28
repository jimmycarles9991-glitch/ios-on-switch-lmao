#!/bin/sh
set -eu

src=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" != 2 ] || [ "$(uname -sm)" != 'Linux aarch64' ] || [ ! -e /.dockerenv ]; then
    echo 'usage in a Linux arm64 container: capture.sh NEW_OUTPUT GLOWALPHA_PROFILE' >&2
    exit 2
fi
profile=$(realpath "$2")
stages="$profile/translated/0000"
mkdir "$1"
out=$(realpath "$1")
build="$out/build"
mkdir "$build" "$out/draws"
cd "$build"
for node in nvmap nvhost-ctrl-gpu nvhost-as-gpu nvhost-gpu nvhost-ctrl; do
    if [ -e "/dev/$node" ]; then test -f "/dev/$node"; else touch "/dev/$node"; fi
done
printf '33\n' >/work/fuse-id
printf '3\n' >/work/fuse-rev
printf 'silicon\n' >/work/fuse-platform
cc -std=gnu11 -Wall -O2 -fPIC -shared -I/work "$src/../shim.c" -ldl -o shim.so
cc -std=gnu11 -Wall -O2 -fPIC -shared -I/work "$src/tap.c" -ldl -pthread -o tap.so
python3 "$src/../layout.py" "$stages/pipeline.json" "$profile/corpus.log" >glow.h
cc -std=gnu11 -Wall -O2 -I. "$src/probe.c" -lvulkan -ldl -o probe
printf '%s\n' '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0","api_version":"1.2.131"}}' >icd.json
glslangValidator -V "$src/probe.vert" -o vertex.spv
glslangValidator -V "$src/probe.frag" -o base.spv
glslangValidator -V -DEARLY "$src/probe.frag" -o early.spv
glslangValidator -V -DEARLY -DPOST "$src/probe.frag" -o post.spv
locks='pixel_interlock_ordered pixel_interlock_unordered sample_interlock_ordered sample_interlock_unordered'
for mode in $locks; do
    glslangValidator -V "-DLOCK_MODE=$mode" "$src/probe.frag" -o "$mode.spv"
done
for shader in ./*.spv "$stages/stage0.r32.spv" "$stages/stage1.r32.spv"; do
    spirv-val --target-env vulkan1.2 --uniform-buffer-standard-layout "$shader"
    sha256sum "$shader"
    spirv-dis "$shader" | grep -E 'OpExecutionMode|BuiltIn SampleId' || true
done >"$out/stages.log"

driver=/l4t/usr/lib/aarch64-linux-gnu
capture() (
    label=$1
    shift
    mkdir "$out/draws/$label"
    cd "$out/draws/$label"
    timeout 20 env LD_LIBRARY_PATH="$driver/tegra:$driver/tegra-egl" LD_PRELOAD="$build/tap.so:$build/shim.so" \
        VK_DRIVER_FILES="$build/icd.json" __EGL_VENDOR_LIBRARY_FILENAMES="$driver/tegra-egl/nvidia.json" \
        __GL_SHADER_DISK_CACHE=0 "$build/probe" "$@" >run.log 2>&1
)
vert="$build/vertex.spv"
frag="$build/base.spv"
capture plain4 base 4 4 0 15 "$vert" "$frag"
capture shaded4 base 4 4 1 15 "$vert" "$frag"
for mode in $locks; do
    for shade in 0 1; do
        capture "${mode}_shade$shade" "$mode" 4 4 "$shade" 15 "$vert" "$build/$mode.spv"
    done
done
capture early4 early 4 4 0 15 "$vert" "$build/early.spv"
capture post4 post 4 4 0 15 "$vert" "$build/post.spv"
capture fill1 base 1 1 0 1 "$vert" "$frag"
capture bbox1 bbox 1 1 0 1 "$vert" "$frag"
for mask in 1 5 15; do
    capture "merge4_1_m$mask" merge 4 1 0 "$mask" "$vert" "$frag"
    capture "base4_m$mask" base 4 4 0 "$mask" "$vert" "$frag"
done
capture glow4 glow 4 4 1 15 "$stages/stage0.r32.spv" "$stages/stage1.r32.spv"
capture glow4_api_off glow 4 4 0 15 "$stages/stage0.r32.spv" "$stages/stage1.r32.spv"
capture truncate4_1 truncate 4 1 0 15 "$vert" "$frag" || {
    status=$?
    [ "$status" = 3 ] && grep -qx 'unsupported combination mode=1 raster=4 depth=4 color=1' \
        "$out/draws/truncate4_1/run.log" || exit "$status"
}
