// SPDX-License-Identifier: MIT
// GM20B colour and depth compression for the raw path's memoryless attachments (Device::backing: the zeta and memoryless
// colour targets). The kernel gives comptags only to BoAlloc memory in physically contiguous, 128 KiB-aligned runs, bound
// with a compressible kind at 128 KiB granularity (TegraGM20BABI.h, drivers/TegraGM20B/TegraGM20BComp.h). Every engine
// reaching memory through such a binding sees plain texels (the L2 decompresses); anything else (the CPU, the display,
// another binding of the same pages) would see compressed bytes. These targets are written and read only by the 3D engine
// and the texture unit, through their one compressed binding, inside a pass: never CPU-mapped, never scanned out, never
// an m2v twin's memory (a memoryless texture's twin has its own), never copied by the copy engine. So none of them is
// ever resolved.
//
// Kinds are NVN's on the Tegra X1 (deko3d pickImageMemoryKind, one sample); the kernel's table (nvkm gm200_mmu_kind)
// demotes each to the kind an uncompressed binding of the same layout takes.
//
// A full-target clear of a compressed target to a value the GPU's ZBC table holds writes comptags and no texels. The table
// is the boot one (nvkm gf100_gr_zbc_init and the kernel's opaque black, TegraGM20BABI.h method 19); zbcBoot* say whether
// a clear is one of its values, for the hit and miss counts that tell whether more entries would pay.
#pragma once
#include <cstdint>
#include <vector>
#include "raw_kernel.h"

namespace raw {

constexpr uint64_t kCompGranule = 1ull << 17;
constexpr uint32_t kCompAlignLog2 = 17;

enum : uint32_t {
    kCompKindC32 = 0xdb,   // NV_MMU_PTE_KIND_C32_2CRA
    kCompKindC64 = 0xe9,   // NV_MMU_PTE_KIND_C64_2CRA
    kCompKindC128 = 0xf5,  // NV_MMU_PTE_KIND_C128_2CR
    kCompKindZeta = 0xce,  // NV_MMU_PTE_KIND_ZF32_X24S8_2CSZV
};

// The compressible kind of a block-linear target of `bpp`-byte texels (the D32S8 zeta when `zeta`), 0 when it has none
inline uint32_t compressedKind(uint32_t bpp, bool zeta)
{
    if (zeta)
        return kCompKindZeta;
    switch (bpp) {
    case 4:
        return kCompKindC32;
    case 8:
        return kCompKindC64;
    case 16:
        return kCompKindC128;
    default:
        return 0;
    }
}

// The kind gm200_mmu_kind demotes a compressible one to
inline uint32_t demotedKind(uint32_t kind)
{
    return kind == kCompKindZeta ? 0xc3u : 0xfeu;
}

inline uint64_t compBytes(uint64_t bytes)
{
    return (bytes + kCompGranule - 1) & ~(kCompGranule - 1);
}

// A colour clear's SET_COLOR_CLEAR_VALUE words (clearWords) for a target of Metal format `mtl`: one of the boot ZBC table's
// values. All zero (FERMI_A_ZBC_COLOR_V0_FMT_ZERO) for any format; for 8-bit RGBA and BGRA unorm, opaque white
// (UNORM_ONE) and opaque black (the kernel's A8B8G8R8 and A8R8G8B8 entries); for RGBA32Float, 0 and 1 (RF32_GF32_BF32_AF32).
inline bool zbcBootColor(uint32_t mtl, const uint32_t words[4])
{
    const uint32_t one = 0x3f800000;
    if (!(words[0] | words[1] | words[2] | words[3]))
        return true;
    const bool white = words[0] == one && words[1] == one && words[2] == one && words[3] == one;
    const bool black = !(words[0] | words[1] | words[2]) && words[3] == one;
    if (mtl == 70 || mtl == 71 || mtl == 80 || mtl == 81)  // RGBA8Unorm(_sRGB), BGRA8Unorm(_sRGB)
        return white || black;
    return mtl == 125 && white;  // RGBA32Float
}

// A depth clear's value: the boot table holds 0.0 and 1.0
inline bool zbcBootDepth(float depth)
{
    return depth == 0.0f || depth == 1.0f;
}

// Compressed memory made off the render thread (Device::compThread_) for the backings that asked for it
struct CompWant {
    uint64_t bytes = 0;
    uint32_t kind = 0;

    bool operator==(const CompWant &o) const
    {
        return bytes == o.bytes && kind == o.kind;
    }
};

struct CompBo {
    Bo bo;
    uint32_t kind = 0;
    uint64_t madeAt = 0;  // Device::commitCount_ when it became ready
};

struct CompQueue {
    std::vector<CompWant> wanted;  // asked for, not made yet
    std::vector<CompBo> ready;     // made and bound, no backing took it yet
    std::vector<CompWant> failed;  // the kernel could not make or compress it: never asked for again
    bool loggedOn = false;

    bool asked(const CompWant &w) const;
};

}  // namespace raw
