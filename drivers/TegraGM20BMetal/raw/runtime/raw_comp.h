// SPDX-License-Identifier: MIT
// GM20B colour and depth compression for the raw path's memoryless attachments (Device::backing: the zeta and memoryless
// colour targets). The kernel gives comptags only to BoAlloc memory in physically contiguous, 128 KiB-aligned runs, bound
// with a compressible kind at 128 KiB granularity (TegraGM20BABI.h, drivers/TegraGM20B/TegraGM20BComp.h): a Compressible
// BO, 128 KiB chunks of the kernel's GPU pool, which a fragmented pool serves as well as an empty one. Every engine
// reaching memory through such a binding sees plain texels (the L2 decompresses); anything else (the CPU, the display,
// another binding of the same pages) would see compressed bytes. These targets are written and read only by the 3D engine
// and the texture unit, through their one compressed binding, inside a pass: never CPU-mapped, never scanned out, never
// an m2v twin's memory (a memoryless texture's twin has its own), never copied by the copy engine. So none of them is
// ever resolved. A texture the app made does not qualify while it can reach m2v (a replayed command buffer twins it in
// place) or a blit (the copy engine): both name its memory through another binding.
//
// Kinds are NVN's on the Tegra X1 (deko3d pickImageMemoryKind, one sample); the kernel's table (nvkm gm200_mmu_kind)
// demotes each to the kind an uncompressed binding of the same layout takes.
//
// A full-target clear of a compressed target to a value the GPU's ZBC table holds writes comptags and no texels. The table
// is the boot one (nvkm gf100_gr_zbc_init and the kernel's opaque black, TegraGM20BABI.h method 19); zbcBoot* say whether
// a clear is one of its values, for the hit and miss counts that tell whether more entries would pay.
#pragma once
#include <IOKit/IOReturn.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
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

// The compressible kind of a Device::backing of `bpp`-byte texels with the kernel's compression on: the zeta's, or a
// memoryless colour target's (tags from memorylessTag). A shadow (a lower tag) is filled and drained by the copy engine,
// and a pitch or multisampled backing has no compressible kind here (the kinds above are one sample's).
inline uint32_t backingKind(bool compression, bool blockLinear, bool zeta, uint32_t bpp, uint32_t samples, uint32_t tag,
                            uint32_t memorylessTag)
{
    if (!compression || !blockLinear || samples > 1)
        return 0;
    if (zeta)
        return kCompKindZeta;
    return tag >= memorylessTag ? compressedKind(bpp, false) : 0;
}

// A refusal of compressed memory (the BoAlloc's or the VmBind's kern_return_t, kIOReturnNoSpace for no VA) that asking
// again cannot pass: anything but the pool, the comptags or the VA running short, all of which come back as memory is freed.
inline bool compLasting(int kr)
{
    return kr != kIOReturnNoMemory && kr != kIOReturnNoResources && kr != kIOReturnNoSpace;
}

constexpr uint64_t kCompRetryCommits = 30, kCompRetryMaxCommits = 600;

// Command buffers from the `refusals`-th refusal of a want to its next ask: half a second of frames, doubling to ten
inline uint64_t compRetryAfter(uint32_t refusals)
{
    uint64_t after = kCompRetryCommits;
    for (uint32_t i = 1; i < refusals && after < kCompRetryMaxCommits; i++)
        after *= 2;
    return std::min(after, kCompRetryMaxCommits);
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

// `bytes` in whole `granule`s, rounded up in quarter steps between powers of two, so the next few larger sizes fit what one
// grows to and a class wastes under a quarter of itself
inline uint64_t sizeClassBytes(uint64_t bytes, uint64_t granule)
{
    const uint64_t units = (bytes + granule - 1) / granule;
    uint64_t top = 1;
    while (top * 2 <= units)
        top *= 2;
    const uint64_t step = std::max<uint64_t>(top / 4, 1);
    return (units + step - 1) / step * step * granule;
}

// The compressed memory asked for an image of `bytes`: whole comptag lines in size classes
inline uint64_t compClassBytes(uint64_t bytes)
{
    return sizeClassBytes(bytes, kCompGranule);
}

// Unused this long (command buffers, or time for the backings that hold compressed memory), a backing or a made buffer no
// backing took goes: the commit rule alone keeps everything an idle process made
constexpr uint64_t kCompIdleCommits = 600, kCompIdleNs = 10'000'000'000;
// The compressed-memory thread's soonest look again: what was due and could not go yet (the command buffer being encoded
// holds it, or the GPU has not finished with it)
constexpr uint64_t kCompRecheckNs = 1'000'000'000;

inline uint64_t compClock()
{
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Whether a backing is stale `now`, with `commits` command buffers committed and `closed` committed or abandoned: last asked
// for at commit count `lastCommit` and closed count `lastClosed`, last committed at `usedNs`. A retired one (no pass takes it
// again) is at once; any after 600 command buffers; one on memory its tag's backings share (compressed, or a memoryless one's
// plain memory: Device::PlainMem) after kCompIdleNs too. One asked for since the last close is in the command buffer being
// encoded: never stale.
inline bool backingStale(uint64_t commits, uint64_t lastCommit, uint64_t closed, uint64_t lastClosed, uint64_t now,
                         uint64_t usedNs, bool retired, bool shared)
{
    if (lastClosed >= closed)
        return false;
    return retired || commits - lastCommit > kCompIdleCommits || (shared && now - usedNs > kCompIdleNs);
}

// Whether a made buffer no backing took, made at commit count `madeAt` and `readyNs`, is stale
inline bool readyStale(uint64_t commits, uint64_t madeAt, uint64_t now, uint64_t readyNs)
{
    return commits - madeAt > kCompIdleCommits || now - readyNs > kCompIdleNs;
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
    uint64_t madeAt = 0;   // Device::commitCount_ when it became ready
    uint64_t readyNs = 0;  // and compClock()
};

// Compressed memory under the backings of one tag and kind whose image fits it, each its own view (texture header, layout)
// of the same bytes. Memoryless contents do not outlive their pass, a pass starts behind a wait for idle
// (RenderEncoder::end's prologue) and a device has one channel, so no two passes use it at once; two backings of one pass
// have different tags. Its buffer goes back to the kernel when the last view does.
struct CompMem {
    Bo bo;
    uint32_t kind = 0;
    uint32_t tag = 0;
    uint32_t views = 0;
};

struct CompRefusal {
    CompWant want;
    uint64_t retryAt = 0;  // the commit count from which it is asked again
    uint32_t refusals = 0;
    bool lasting = false;  // never asked again
};

// Times are Device::commitCount_
struct CompQueue {
    std::vector<CompWant> wanted;      // asked for, not made yet
    std::vector<CompBo> ready;         // made and bound, no backing took it yet
    std::vector<CompRefusal> refused;  // the kernel's last answer to it was no
    std::vector<std::unique_ptr<CompMem>> mems;
    bool loggedOn = false;

    // Not to be asked for at `now`: asked already or made at least as large, or refused and not due again
    bool asked(const CompWant &w, uint64_t now) const
    {
        const auto covers = [&](uint64_t bytes, uint32_t kind) {
            return kind == w.kind && bytes >= w.bytes;
        };
        if (std::any_of(wanted.begin(), wanted.end(),
                        [&](const CompWant &x) {
                            return covers(x.bytes, x.kind);
                        }) ||
            std::any_of(ready.begin(), ready.end(), [&](const CompBo &b) {
                return covers(b.bo.bytes, b.kind);
            }))
            return true;
        auto r = find(w);
        return r != refused.end() && (r->lasting || now < r->retryAt);
    }

    // The memory new backings of `tag` and `kind` go on: the largest of them; null with none
    CompMem *largest(uint32_t tag, uint32_t kind) const
    {
        CompMem *best = nullptr;
        for (const auto &m : mems)
            if (m->tag == tag && m->kind == kind && (!best || m->bo.bytes > best->bo.bytes))
                best = m.get();
        return best;
    }

    // The smallest made buffer of `kind` holding `bytes`, or ready.end()
    std::vector<CompBo>::iterator fit(uint64_t bytes, uint32_t kind)
    {
        auto best = ready.end();
        for (auto it = ready.begin(); it != ready.end(); ++it)
            if (it->kind == kind && it->bo.bytes >= bytes && (best == ready.end() || it->bo.bytes < best->bo.bytes))
                best = it;
        return best;
    }

    // The kernel refused `w` at `now`: true at its first refusal since it was last made
    bool refuse(const CompWant &w, uint64_t now, bool lasting)
    {
        auto r = find(w);
        const bool first = r == refused.end();
        if (first)
            r = refused.insert(refused.end(), CompRefusal{w});
        r->refusals++;
        r->retryAt = now + compRetryAfter(r->refusals);
        r->lasting = r->lasting || lasting;
        return first;
    }

    void made(const CompWant &w)
    {
        if (auto r = find(w); r != refused.end())
            refused.erase(r);
    }

    // Compressed memory went back to the kernel at `now`: every refusal that asking again can pass is due
    void freed(uint64_t now)
    {
        for (CompRefusal &r : refused)
            r.retryAt = std::min(r.retryAt, now);
    }

private:
    std::vector<CompRefusal>::iterator find(const CompWant &w)
    {
        return std::find_if(refused.begin(), refused.end(), [&](const CompRefusal &r) {
            return r.want == w;
        });
    }

    std::vector<CompRefusal>::const_iterator find(const CompWant &w) const
    {
        return std::find_if(refused.begin(), refused.end(), [&](const CompRefusal &r) {
            return r.want == w;
        });
    }
};

}  // namespace raw
