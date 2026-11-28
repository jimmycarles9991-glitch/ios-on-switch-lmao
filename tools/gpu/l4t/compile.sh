#!/bin/sh
set -eu
src=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ "$(uname -sm)" != "Linux aarch64" ] || [ ! -e /.dockerenv ]; then
    echo 'Run in a linux/arm64 Docker container with /work writable and /l4t mounted read-only.' >&2
    exit 1
fi
case $# in
    1) out=$1 ;;
    3)
        out=$1
        capture=$(realpath "$2")
        corpus=$(realpath "$3")
        ;;
    *)
        echo 'usage: compile.sh OUTPUT [TRANSLATED_STAGE_DIR CORPUS_LOG]' >&2
        exit 1
        ;;
esac
mkdir -p "$out"
cd "$out"
for node in nvmap nvhost-ctrl-gpu nvhost-as-gpu nvhost-gpu nvhost-ctrl; do
    file="/dev/$node"
    if [ -e "$file" ]; then test -f "$file"; else touch "$file"; fi
done
printf '33\n' >/work/fuse-id
printf '3\n' >/work/fuse-rev
printf 'silicon\n' >/work/fuse-platform
cc -std=gnu11 -Wall -O2 -fPIC -shared -I/work "$src/shim.c" -ldl -o shim.so
if [ "$#" = 3 ]; then
    python3 "$src/layout.py" "$capture/pipeline.json" "$corpus" >case.h
    vert="$capture/stage0.r32.spv"
    frag="$capture/stage1.r32.spv"
else
    python3 "$src/layout.py" >case.h
    glslangValidator -V "$src/probe.vert" -o vert.spv
    glslangValidator -V "$src/probe.frag" -o frag.spv
    vert=vert.spv
    frag=frag.spv
fi
cc -std=gnu11 -Wall -O2 -I. "$src/pipeline.c" -lvulkan -o pipeline
sha256sum "$vert" | cut -d " " -f1 >stage0.sha256
sha256sum "$frag" | cut -d " " -f1 >stage1.sha256
driver=/l4t/usr/lib/aarch64-linux-gnu
printf '%s\n' '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0","api_version":"1.2.131"}}' >icd.json
env LD_LIBRARY_PATH="$driver/tegra:$driver/tegra-egl" LD_PRELOAD="$PWD/shim.so" \
    VK_DRIVER_FILES="$PWD/icd.json" __EGL_VENDOR_LIBRARY_FILENAMES="$driver/tegra-egl/nvidia.json" \
    __GL_SHADER_DISK_CACHE=0 ./pipeline "$vert" "$frag" >statistics.txt
python3 "$src/unpack.py" pipeline.cache .
for stage in vert frag; do
    "${ENVYDIS:-/work/envydis}" -m gm107 -V sm52 -F sm60op -n -i "$stage.sass" >"$stage.asm"
done
cat statistics.txt
