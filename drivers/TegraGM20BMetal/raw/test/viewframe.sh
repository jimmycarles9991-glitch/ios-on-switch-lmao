#!/bin/bash
# raw GM20B path: the view_* kernels through m2v + NVK and through the raw facade (test/viewframe.mm), compared dispatch by
# dispatch (pack/eqcheck.py over nvdec's DISPATCH records: each texture slot's header, all eight words).
# usage: viewframe.sh <build dir (test/build.sh's)> <pack.bin> <out dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd -P)
B=$1
PACK=$2
O=$3
NVK=${NVK:-${HERE}/../../../../bazel-bin/gpu/nvk/host/libvulkan_nouveau.dylib}
mkdir -p "$O/cache"
export MESA_SHADER_CACHE_DISABLE=true NMB_ASYNC_PSO=0 NMB_ASYNC_PSO_API=0 NVDEC_REGS=1
RAWCAP_LOG=/dev/null "$B/viewframe" nvk "$NVK" "$O/cache" "$B/HWTest.metallib" >"$O/warm.out" 2>"$O/warm.err"
python3 "$HERE/../pack/mkprograms.py" "$O/programs.bin" "$O/cache" >/dev/null
export RAWCAP_PROGRAMS=$O/programs.bin
# m2v writes only its layout's words of a set: scrubbed after each decode, no word of an earlier dispatch shows in a later one
NVDEC_KERNELS=1 NVDEC_SCRUB=1 RAWCAP_LOG=$O/nvk.log "$B/viewframe" nvk "$NVK" "$O/cache" "$B/HWTest.metallib" >"$O/nvk.out" 2>"$O/nvk.err"
status=0
RAWCAP_LOG=$O/raw.log "$B/viewframe" facade "$NVK" "$O/cache" "$B/HWTest.metallib" "$PACK" >"$O/raw.out" 2>"$O/raw.err" \
    || status=$?
cat "$O/raw.out"
[[ ${status} -eq 0 ]] || exit "${status}"
python3 "$HERE/../pack/eqcheck.py" "$O/nvk.log" "$O/raw.log" --flat --own --show 3 || true
# each dispatch's set 0 by content (handles by their headers, all eight words): eqcheck also names each texture by its first
# appearance, which m2v's clears of new textures move
grep -o '^  cb0 root set0=[0-9a-f]*' "$O/nvk.log" >"$O/nvk.sets" || true
grep -o '^  cb0 root set0=[0-9a-f]*' "$O/raw.log" >"$O/raw.sets" || true
if [[ -s "$O/nvk.sets" ]] && cmp -s "$O/nvk.sets" "$O/raw.sets"; then
    echo "viewframe: every dispatch's set identical"
else
    echo "viewframe: sets differ"
    exit 1
fi
