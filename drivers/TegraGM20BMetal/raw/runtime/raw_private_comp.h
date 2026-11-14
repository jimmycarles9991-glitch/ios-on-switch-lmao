// SPDX-License-Identifier: MIT
// Private render targets on compressed memory (rule comp.private). A private 2D texture of one sample (its mip chain too)
// or 2D array of one level, or a stored multisampled one, that only the 3D engine renders, shaders store to and the texture
// unit samples is bound with its texel size's compressible kind when
// compressed memory of its size is ready (Device::comp_, made off the render thread), so its full clears to a ZBC value
// write tags only and its draws move compressed tiles. The kind must be one the copy engine reads and writes as plain
// texels (raw_comp.h copyEngineReadsCompressed: the move below is its copy); passes that view it as a storage image do so
// in place (raw_storage_form.h rule storage.compressed).
//
// Raw views and gpuResourceIDs name the compressed memory through headers of their own over the same binding. An m2v twin
// may not: it aliases the pages through NVK's plain binding and would read compressed bytes. So before a twin is taken
// the texture moves to plain block-linear memory: the copy engine copies it there in a raw job, ordered before every later
// raw job by the channel and before NVK's next submission by Order, and the same job then rewrites the texture's header
// and each raw view's in place to name the plain memory (ticAt), so a gpuResourceID or view handed out before keeps its
// index and reads what later work writes. Work already submitted reads the old memory, which stays until the texture goes.
#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>
#include "raw_comp.h"
#include "raw_kernel.h"

namespace raw {

enum class PrivateShape2D : uint8_t {
    Plain,        // a 2D texture
    Array,        // a 2D array
    Multisample,  // a 2D multisampled texture
    Other,
};

struct PrivateTextureFacts {
    bool privateStorage = false;
    PrivateShape2D shape = PrivateShape2D::Other;
    uint32_t levels = 1, samples = 1;
    // usage is RenderTarget, ShaderRead and/or ShaderWrite (the surface unit's stores go through the binding's kind, rule
    // storage.compressed), nothing else (not Unknown)
    bool compressibleUsage = false;
    bool plainColour = false;  // a colour format the 3D engine renders, no extended-range or planar form
    uint32_t bpp = 0;
    bool compression = false;  // the kernel compresses (TegraGM20BInfoCompression)
};

// Whether a private texture of this shape is laid out on compressed memory where its kind allows
constexpr bool privateShapeCompresses(const PrivateTextureFacts &f)
{
    switch (f.shape) {
    case PrivateShape2D::Plain:  // a mipmapped one is a chain (Texture::chain) the 2D engine fills from level 0
        return f.samples <= 1;
    case PrivateShape2D::Array:
        return f.levels == 1 && f.samples <= 1;
    case PrivateShape2D::Multisample:  // createStoredMultisample's: its kind is the sample count's
        return f.levels == 1 && (f.samples == 2 || f.samples == 4);
    default:
        return false;
    }
}

// The compressed kind such a texture binds, or 0
inline uint32_t privateCompressedKind(const PrivateTextureFacts &f)
{
    if (!f.compression || !f.privateStorage || !privateShapeCompresses(f) || !f.compressibleUsage || !f.plainColour)
        return 0;
    const uint32_t kind = compressedKind(f.bpp, 0, std::max(f.samples, 1u));
    return copyEngineReadsCompressed(kind) ? kind : 0;
}

// The address a block-linear texture header (fx::mipChainTic's) names: ADDRESS_BITS31TO9 (word 1 bits 31:9) and
// ADDRESS_BITS47TO32 (word 2 bits 15:0)
constexpr uint64_t ticAddress(const uint32_t w[8])
{
    return uint64_t(w[1] >> 9) << 9 | uint64_t(w[2] & 0xffff) << 32;
}

// The same header naming `va` (512-byte aligned, as every block-linear image is)
constexpr void ticAt(uint32_t w[8], uint64_t va)
{
    w[1] = (w[1] & 0x1ffu) | uint32_t(va & 0xfffffe00u);
    w[2] = (w[2] & ~0xffffu) | uint32_t(va >> 32 & 0xffff);
}

// fx::msLayout's grid of `samples` samples as log2 columns and rows: the image the copy engine moves a multisampled
// texture as, one sample a texel
constexpr void sampleGrid(uint32_t samples, uint32_t &sxLog2, uint32_t &syLog2)
{
    sxLog2 = samples >= 2 ? 1 : 0;
    syLog2 = samples >= 4 ? 1 : 0;
}

// The colour slots a draw binds as storage images where its state has no barrier form to take instead, or one that
// cannot draw its geometry (`twinDraws`: raw_fetch_order.h barrierFormDraws; boot 21's RenderBox quads, indexed strips of
// four): a compressed attachment there refuses the draw unless storage views may name compressed memory
// (`compressedOk`: Device::storageCompressedOk, raw_storage_form.h), so it moves to plain memory before the range
// encodes. A barrier form that draws it keeps the compressed attachment.
constexpr uint8_t storageSlotsToPlain(uint8_t storageBits, bool barrierTwin, bool twinDraws, bool compressedOk)
{
    return (barrierTwin && twinDraws) || compressedOk ? 0 : storageBits;
}

// What a target is made for, by its descriptor: a target of the same shape as one a storage form needed plain is made
// plain, so an owner that makes such a target again (a compositor's per-frame target) moves none
struct PrivateShape {
    uint64_t format = 0, width = 0, height = 0, usage = 0;

    bool operator<(const PrivateShape &o) const
    {
        return std::tie(format, width, height, usage) < std::tie(o.format, o.width, o.height, o.usage);
    }
};

class PlainShapes {
public:
    bool holds(const PrivateShape &s) const
    {
        return shapes_.count(s) != 0;
    }

    void add(const PrivateShape &s)
    {
        shapes_.insert(s);
    }

private:
    std::set<PrivateShape> shapes_;
};

struct PrivateComp {
    Bo bo;
};

}  // namespace raw
