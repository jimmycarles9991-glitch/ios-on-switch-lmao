// SPDX-License-Identifier: MIT
// Private render targets on compressed memory (rule comp.private). A private 2D texture of one sample (its mip chain
// too) or 2D array of one level, or a stored multisampled one, that only the 3D engine renders, shaders store to and
// the texture unit samples is bound with its texel size's compressible kind when compressed memory of its size is ready
// (Device::comp_, made off the render thread), so its full clears to a ZBC value write tags only and its draws move
// compressed tiles. The kind must be one the copy engine reads and writes as plain texels (comp.h
// copyEngineReadsCompressed: the move below is its copy); passes that view it as a storage image do so in place
// (storage_form.h rule storage.compressed). The driver's views and gpuResourceIDs name the compressed memory through headers
// of their own over the same binding, and nothing else binds its pages, so it stays on that memory until it goes.
//
// A private stored one-sample depth/stencil texture of one layer and level binds its format's compressible kind on the
// same terms (comp.h storedZetaKind, Device::createOwnedZeta).
//
// A private target or depth texture made plain because no compressed memory of its size was ready at its creation wants
// that memory (Device::wantCompressed): any of the shapes above but a mip chain, whose level headers no move rewrites.
// Where memory that serves it is made or kept, the submit worker's next raw range submission moves it, before that
// range's encode and whether or not the range uses it (Device::compressedFromPlain): the copy engine copies it in a raw
// job of its own, every layer and sample, which then rewrites its header and those made from it in place (ticAt), so a
// gpuResourceID or view handed out before keeps its index, and its plain memory goes once that job completed. The
// worker alone writes its memory's fields, under lock_; no other thread reads them but under lock_ (view creation) or
// once no move of it can come (a CPU copy, which withdraws a want still there on the worker first: Device::keepPlain).
// One with a raw view stays plain: a view may go, and its header index be reused, before the job runs.
#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>
#include "comp.h"
#include "kernel.h"

namespace gm20b {

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
    uint32_t zeta = 0;         // a depth/stencil texture's Metal format (zeta.h), else 0
    uint32_t layers = 1;       // an array's or a cube's (six a cube)
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
    if (!f.compression || !f.privateStorage || !f.compressibleUsage)
        return 0;
    if (f.zeta)
        return f.levels == 1 ? storedZetaKind(true, f.zeta, f.samples, f.layers) : 0;
    if (!privateShapeCompresses(f) || !f.plainColour)
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
    sxLog2 = samples >= 8 ? 2 : samples >= 2 ? 1 : 0;
    syLog2 = samples >= 4 ? 1 : 0;
}

struct PrivateComp {
    Bo bo;
    uint32_t kind = 0;  // bo's: when the texture goes, bo joins the made buffers (CompQueue::ready) for its kind
};

}  // namespace gm20b
