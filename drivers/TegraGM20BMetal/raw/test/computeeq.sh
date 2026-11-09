#!/bin/bash
# raw GM20B path: one Metal kernel's dispatch through m2v + NVK and through the raw compute encoder, compared record by record
# (pack/eqcheck.py over nvdec's DISPATCH records). An NVK run's NVDEC_KERNELS lines are the kernel's record the raw run
# dispatches; a first NVK run fills the cache its programs.bin is made from (pack/mkprograms.py).
# usage: computeeq.sh <build dir> <pack.bin> <metallib> <kernel> <threads x,y,z> <groups x,y,z> <out dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd -P)
B=$1
PACK=$2
LIB=$3
KERNEL=$4
THREADS=$5
GRID=$6
O=$7
NVK=${NVK:-${HERE}/../../../../bazel-bin/gpu/nvk/host/libvulkan_nouveau.dylib}
mkdir -p "$O/cache"
export MESA_SHADER_CACHE_DISABLE=true NMB_ASYNC_PSO=0 NMB_ASYNC_PSO_API=0 NVDEC_REGS=1
RAWCAP_LOG=/dev/null "$B/computeframe" nvk "$NVK" "$O/cache" "$LIB" "$KERNEL" "$THREADS" "$GRID" 2>"$O/warm.err"
python3 "$HERE/../pack/mkprograms.py" "$O/programs.bin" "$O/cache" >/dev/null
export RAWCAP_PROGRAMS=$O/programs.bin
# the kernel's record from a run with every slot tagged (FRAME_BIG_BUFFER binds untagged bytes)
env NVDEC_KERNELS=1 RAWCAP_LOG="$O/record.log" FRAME_BIG_BUFFER= "$B/computeframe" nvk "$NVK" "$O/cache" "$LIB" "$KERNEL" "$THREADS" "$GRID" \
    2>"$O/record.err"
RAWCAP_LOG=$O/nvk.log "$B/computeframe" nvk "$NVK" "$O/cache" "$LIB" "$KERNEL" "$THREADS" "$GRID" 2>"$O/nvk.err"
# PACK_KEY=<raw_compute_key.h key>: the raw run dispatches the pack's kernel of that key instead
if [[ -n "${PACK_KEY:-}" ]]; then
    RAWCAP_LOG=$O/raw.log "$B/computeframe" pack "$PACK" - "$PACK_KEY" "$THREADS" "$GRID" 2>"$O/raw.err"
else
    RAWCAP_LOG=$O/raw.log "$B/computeframe" raw "$PACK" "$O/record.log" "$O/programs.bin" "$THREADS" "$GRID" 2>"$O/raw.err"
fi
python3 "$HERE/../pack/eqcheck.py" "$O/nvk.log" "$O/raw.log" --flat --own --show 3
