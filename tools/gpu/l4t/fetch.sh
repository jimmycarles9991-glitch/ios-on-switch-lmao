#!/bin/sh
set -eu
if [ "$#" != 1 ]; then
    echo 'usage: fetch.sh SCRATCH' >&2
    exit 1
fi
mkdir -p "$1/packages" "$1/run" "$1/l4t"
cd "$1"
fetch() {
    df -h /
    available=$(df -Pk . | awk 'NR==2 {print $4}')
    test "$available" -gt 6500000 || {
        echo 'Less than 6 GiB plus extraction headroom' >&2
        exit 1
    }
    curl -fLsS --retry 2 "$1" -o "$2.partial"
    printf '%s  %s\n' "$3" "$2.partial" | shasum -a 256 -c -
    mv "$2.partial" "$2"
}
version=32.7.6-20241104234540_arm64
origin=https://repo.download.nvidia.com/jetson/t210/pool/main/n
for package in nvidia-l4t-core nvidia-l4t-3d-core; do
    case "$package" in
        nvidia-l4t-core) digest=61ada8308bd38fe6fe1793b658c83c3ef3dcc057a0d9e080dc0336e61be683c2 ;;
        nvidia-l4t-3d-core) digest=44df4b465417916ac1bdb1c801892c7af948114cadf5a3a1eafb780984600a94 ;;
    esac
    name="${package}_${version}.deb"
    fetch "$origin/$package/$name" "packages/$name" "$digest"
    if command -v dpkg-deb >/dev/null 2>&1; then
        dpkg-deb -x "packages/$name" l4t
    else
        member=$(tar -tf "packages/$name" | awk '/^data\.tar(\.|$)/ {print $1}')
        test -n "$member"
        tar -xOf "packages/$name" "$member" >packages/l4t-data.tar
        tar -xf packages/l4t-data.tar -C l4t
        rm packages/l4t-data.tar
    fi
    rm "packages/$name"
done
raw=https://raw.githubusercontent.com/OE4T
fetch "$raw/linux-nvgpu/7fbefcbf4b39643ab414454e6e4154865b333757/include/uapi/linux/nvgpu.h" run/nvgpu.h \
    25a1986412a9dbbd770bf5e2035890701e731f79d10ebc14e05724926237254a
fetch "$raw/linux-tegra-4.9/6944a5ce1dae5947ff24ada6b6592d9e3062d38c/nvidia/include/uapi/linux/nvmap.h" run/nvmap.h \
    341303eb7ca8267b5bda1c1e780b6339e45d61b051ce1b87760c9516972c8be5
