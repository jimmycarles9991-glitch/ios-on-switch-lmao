// SPDX-License-Identifier: MIT
// Private render targets on compressed memory (rule comp.private). A private 2D texture of one level and one sample that
// only the 3D engine renders and the texture unit samples is bound with C32_2CRA or C64_2CRA when compressed memory of
// its size is ready (Device::comp_, made off the render thread), so its full clears to a ZBC value write tags only and
// its draws move compressed tiles. Those kinds are the ones the copy engine and the texture unit read and write as plain
// texels on the chip (HW Test gpu.x1.ce_compressed, boot 6 stack-F-v1: BGRA8 and RGBA16Float, 0 bad of 24 repeats);
// passes that would view it as a storage image take the state's barrier form (raw_storage_form.h).
//
// Nothing else may name its pages: an m2v twin aliases them through NVK's plain binding and would read compressed bytes.
// So before a twin, a view (which copies the memory's address) or a gpuResourceID (which names the header) is taken, the
// texture moves to plain block-linear memory: the copy engine copies it there in a raw job, ordered before every later
// raw job by the channel and before NVK's next submission by Order, and later work binds a new header over the plain
// memory. Work already submitted keeps the old header and memory, which stay until the texture goes.
#pragma once
#include <cstdint>
#include <set>
#include <tuple>
#include "raw_comp.h"
#include "raw_kernel.h"

namespace raw {

struct PrivateTextureFacts {
    bool privateStorage = false;
    bool oneLevel = false, oneSample = false, twoD = false;
    bool renderedAndSampledOnly = false;  // usage is RenderTarget and/or ShaderRead, nothing else (not Unknown)
    bool plainColour = false;             // a colour format the 3D engine renders, no extended-range or planar form
    uint32_t bpp = 0;
    bool compression = false;  // the kernel compresses (TegraGM20BInfoCompression)
};

// The compressed kind such a texture binds, or 0
constexpr uint32_t privateCompressedKind(const PrivateTextureFacts &f)
{
    if (!f.compression || !f.privateStorage || !f.oneLevel || !f.oneSample || !f.twoD || !f.renderedAndSampledOnly ||
        !f.plainColour)
        return 0;
    return f.bpp == 4 ? kCompKindC32 : f.bpp == 8 ? kCompKindC64 : 0;
}

// The colour slots a draw binds as storage images where its state has no barrier form to take instead: a compressed
// attachment there refuses the draw (raw_storage_form.h, the surface unit reads a compressible kind's raw bytes), so it
// moves to plain memory before the range encodes. A state with a barrier form keeps the compressed attachment and draws
// through that form.
constexpr uint8_t storageSlotsToPlain(uint8_t storageBits, bool barrierTwin)
{
    return barrierTwin ? 0 : storageBits;
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
    uint32_t staleTic = 0;  // the header work submitted before the move binds; freed with the texture
};

}  // namespace raw
