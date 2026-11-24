// SPDX-License-Identifier: MIT
// Rule barrier.pass_pixel: a pass opens with PIXEL_SHADER_BARRIER and its pixel-ordered texture invalidate (0x0f74), in
// place of today's drain, where every dependency it has on the passes since the last drain is a cell
// gpu.x1.pass_pixel_barrier held in: claim PASS on boots 8, 14, 15, 19 and 21, boot 21 22 cells held, none failed, 91,294
// passes (kHeldCells, that line's pixel_cells). A cell is a format, a layout, the read's shape and a size; a pass shows
// all of those but the shape of its samples, which only its program's arithmetic says. So a read of an earlier pass's
// target is held where every shape was held at its size (64x64 block-linear), or where the pass draws a pyramid level of
// it (a target at most half its size), or a write over it (waw); every other dependency, the 8 blind cells' sizes among
// them, and anything a pass does that no cell did (storage writes, depth, more than one sample, level or layer,
// vertex-stage samples, an unnamed or fenced read, a target sampled before it is written) keeps the drain. A memoryless
// attachment's backing is scratch: Metal leaves its contents undefined at every pass's start, so a later pass's scratch
// over the same memory depends on nothing an earlier pass left there. Every pass the rule opens begins with
// PIXEL_SHADER_BARRIER, which holds its pixel shading until the earlier passes' pixels have left the ROP, so their
// scratch writes and in-pass fetches of it are done before its own begin; its first fetch of a cleared or DontCare
// attachment takes the in-pass barrier and its texture invalidate (RenderEncoder::begin's whole-area write). Scratch over
// memory a real image of the chain names, either way, keeps the drain.
#pragma once
#include <cstdint>
#include <iterator>
#include "attachment_lifetime.h"

namespace gm20b {

enum class HazardLayout : uint8_t {
    BlockLinear,
    Pitch,
    Compressed
};

enum class HazardShape : uint8_t {
    Same,
    Stride,
    Flip,
    Pyramid,
    Waw
};

enum class HazardSize : uint8_t {
    Small,   // 64 x 64 at most (the probe's "s")
    Medium,  // 320 x 180 at most ("m")
    Large    // the rest: 1280 x 720 ("f") held in no cell
};

struct HeldCell {
    uint32_t format;  // MTLPixelFormat
    HazardLayout layout;
    HazardShape shape;
    HazardSize size;
};

constexpr uint32_t kHazardBgra8 = 80, kHazardRgba16f = 115;

// gpu.x1.pass_pixel_barrier boot 21 (-20261113-134714) pixel_cells' H entries, BGRA8 then RGBA16F alike
constexpr HeldCell kHeldCells[] = {
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Same, HazardSize::Small},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Same, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Stride, HazardSize::Small},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Flip, HazardSize::Small},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Pyramid, HazardSize::Small},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::Pitch, HazardShape::Same, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::Pitch, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::Compressed, HazardShape::Same, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::Compressed, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardBgra8, HazardLayout::BlockLinear, HazardShape::Waw, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Same, HazardSize::Small},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Same, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Stride, HazardSize::Small},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Flip, HazardSize::Small},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Pyramid, HazardSize::Small},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::Pitch, HazardShape::Same, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::Pitch, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::Compressed, HazardShape::Same, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::Compressed, HazardShape::Pyramid, HazardSize::Medium},
    {kHazardRgba16f, HazardLayout::BlockLinear, HazardShape::Waw, HazardSize::Medium},
};

constexpr bool heldCell(uint32_t format, HazardLayout layout, HazardShape shape, HazardSize size)
{
    for (const HeldCell &c : kHeldCells)
        if (c.format == format && c.layout == layout && c.shape == shape && c.size == size)
            return true;
    return false;
}

// One image a pass reads or writes: its memory and what a cell is keyed by. `plain`: one sample, level, layer and depth
// plane of a colour format, with memory of its own
struct HazardImage {
    uint64_t va = 0, bytes = 0;
    uint64_t importBase = 0, importBytes = 0;  // the surface or no-copy pages under it
    uint32_t format = 0, width = 0, height = 0;
    HazardLayout layout = HazardLayout::BlockLinear;
    bool plain = false;
};

template <class Texture> HazardImage hazardImage(const Texture &t)
{
    HazardImage i;
    i.va = t.va;
    i.bytes = fusionImageBytes(t);
    if (t.import) {
        i.importBase = t.import->base;
        i.importBytes = t.import->bytes;
    }
    i.format = t.format;
    i.width = t.width;
    i.height = t.height;
    i.layout = t.comptags ? HazardLayout::Compressed : t.blockLinear ? HazardLayout::BlockLinear : HazardLayout::Pitch;
    i.plain = t.va && !t.memoryless && !t.storage.gpuVolatile && t.samples == 1 && t.layers == 1 && t.depth == 1 &&
              !t.chain && !t.zeta && !t.planar;
    return i;
}

constexpr bool rangesOverlap(uint64_t a, uint64_t bytesA, uint64_t b, uint64_t bytesB)
{
    return a && b && bytesA && bytesB && (a <= b ? b - a < bytesA : a - b < bytesB);
}

constexpr bool overlaps(const HazardImage &x, const HazardImage &y)
{
    return rangesOverlap(x.va, x.bytes, y.va, y.bytes) ||
           rangesOverlap(x.importBase, x.importBytes, y.importBase, y.importBytes);
}

constexpr HazardSize hazardSize(uint32_t width, uint32_t height)
{
    return width <= 64 && height <= 64     ? HazardSize::Small
           : width <= 320 && height <= 180 ? HazardSize::Medium
                                           : HazardSize::Large;
}

// A sample of `read`, which an earlier pass wrote, by a pass drawing `areaWidth` x `areaHeight`: held where every shape
// its samples could take was, or where the pass is a pyramid level of it
constexpr bool heldRead(const HazardImage &read, uint32_t areaWidth, uint32_t areaHeight)
{
    if (!read.plain)
        return false;
    const HazardSize size = hazardSize(read.width, read.height);
    const bool pyramid =
        areaWidth && areaHeight && 2ull * areaWidth <= read.width + 1ull && 2ull * areaHeight <= read.height + 1ull;
    if (pyramid && heldCell(read.format, read.layout, HazardShape::Pyramid, size))
        return true;
    for (HazardShape shape : {HazardShape::Same, HazardShape::Stride, HazardShape::Flip, HazardShape::Pyramid})
        if (!heldCell(read.format, read.layout, shape, size))
            return false;
    return true;
}

constexpr bool heldWrite(const HazardImage &written)
{
    return written.plain &&
           heldCell(written.format, written.layout, HazardShape::Waw, hazardSize(written.width, written.height));
}

constexpr uint32_t kPassImages = 16, kChainImages = 128;

// Why a known pass keeps the drain (drainCause)
enum class DrainCause : uint8_t {
    None,
    Unknown,       // the pass is not known
    Chain,         // a pass since the last drain was not known, or a list of the chain filled
    Depth,         // a depth or stencil attachment, this pass's or the chain's
    Read,          // it samples what the chain wrote, in no held cell
    Write,         // it writes what the chain wrote, in no held cell
    WriteSampled,  // it writes what the chain sampled
    Scratch        // scratch over memory a real image names
};

// What a pass reads and writes. `known`: everything it reads came through its bindings and named uses (no heap, fence
// wait, storage write or an image past the lists); `vertexSamples`: its vertex stage binds a texture
struct PassUses {
    bool known = false, vertexSamples = false;
    bool shaderWrites = false, bufferWrites = false;
    uint32_t areaWidth = 0, areaHeight = 0;
    HazardImage written[8];  // its colour targets
    uint8_t writtenCount = 0;
    HazardImage scratch[8];  // its memoryless colours' backings
    uint8_t scratchCount = 0;
    bool depthStencil = false;  // it binds a depth or stencil attachment
    HazardImage depth;
    HazardImage sampled[kPassImages];  // what its stages sample and the resources it names (useResource)
    bool named[kPassImages] = {};      // through a use, whose stage it does not say
    uint8_t sampledCount = 0;

    bool write(const HazardImage &image)
    {
        if (writtenCount == std::size(written))
            return known = false;
        written[writtenCount++] = image;
        return true;
    }

    bool scratchWrite(const HazardImage &image)
    {
        if (scratchCount == std::size(scratch))
            return known = false;
        scratch[scratchCount++] = image;
        return true;
    }

    bool sample(const HazardImage &image, bool throughUse)
    {
        if (sampledCount == kPassImages)
            return known = false;
        named[sampledCount] = throughUse;
        sampled[sampledCount++] = image;
        return true;
    }
};

// The passes since the last drain: the images they wrote, sampled and used as scratch, each once; `valid` false where
// one of them was not known or a list filled
struct PassChain {
    bool valid = false;
    HazardImage written[kChainImages], sampled[kChainImages], scratch[kChainImages];
    uint8_t writtenCount = 0, sampledCount = 0, scratchCount = 0;
    bool depth = false;  // one of them bound a depth or stencil attachment

    void reset()
    {
        valid = depth = false;
        writtenCount = sampledCount = scratchCount = 0;
    }
};

constexpr bool overlapsAny(const HazardImage &image, const HazardImage *list, uint8_t count)
{
    for (uint8_t j = 0; j < count; j++)
        if (overlaps(image, list[j]))
            return true;
    return false;
}

// What keeps `next` from opening with PIXEL_SHADER_BARRIER after the chain: None where its dependencies on it are all
// held cells
constexpr DrainCause drainCause(const PassChain &chain, const PassUses &next)
{
    if (!next.known)
        return DrainCause::Unknown;
    if (!chain.valid)
        return DrainCause::Chain;
    if (next.depthStencil || chain.depth)
        return DrainCause::Depth;
    for (uint8_t w = 0; w < next.writtenCount; w++) {
        const HazardImage &target = next.written[w];
        if (overlapsAny(target, chain.sampled, chain.sampledCount))
            return DrainCause::WriteSampled;
        if (overlapsAny(target, chain.scratch, chain.scratchCount))
            return DrainCause::Scratch;
        for (uint8_t j = 0; j < chain.writtenCount; j++)
            if (overlaps(target, chain.written[j]) && !(heldWrite(target) && heldWrite(chain.written[j])))
                return DrainCause::Write;
    }
    for (uint8_t k = 0; k < next.scratchCount; k++)
        if (overlapsAny(next.scratch[k], chain.written, chain.writtenCount) ||
            overlapsAny(next.scratch[k], chain.sampled, chain.sampledCount))
            return DrainCause::Scratch;
    for (uint8_t s = 0; s < next.sampledCount; s++) {
        const HazardImage &read = next.sampled[s];
        if (overlapsAny(read, chain.scratch, chain.scratchCount))
            return DrainCause::Scratch;
        for (uint8_t j = 0; j < chain.writtenCount; j++) {
            if (!overlaps(read, chain.written[j]))
                continue;
            if (next.named[s] || next.vertexSamples || !heldRead(read, next.areaWidth, next.areaHeight) ||
                !heldRead(chain.written[j], next.areaWidth, next.areaHeight))
                return DrainCause::Read;
        }
    }
    return DrainCause::None;
}

// Whether `next`'s dependencies on the chain are all held cells: PIXEL_SHADER_BARRIER may open it
constexpr bool pixelBarrierHolds(const PassChain &chain, const PassUses &next)
{
    return drainCause(chain, next) == DrainCause::None;
}

constexpr bool passesIndependent(const PassChain &chain, const PassUses &next)
{
    if (!next.known || !chain.valid || next.depthStencil || chain.depth)
        return false;
    for (uint8_t w = 0; w < next.writtenCount; w++) {
        const HazardImage &target = next.written[w];
        if (overlapsAny(target, chain.written, chain.writtenCount) ||
            overlapsAny(target, chain.sampled, chain.sampledCount) ||
            overlapsAny(target, chain.scratch, chain.scratchCount))
            return false;
    }
    for (uint8_t k = 0; k < next.scratchCount; k++) {
        const HazardImage &scratch = next.scratch[k];
        if (overlapsAny(scratch, chain.written, chain.writtenCount) ||
            overlapsAny(scratch, chain.sampled, chain.sampledCount) ||
            overlapsAny(scratch, chain.scratch, chain.scratchCount))
            return false;
    }
    for (uint8_t s = 0; s < next.sampledCount; s++) {
        const HazardImage &read = next.sampled[s];
        if (overlapsAny(read, chain.written, chain.writtenCount) ||
            overlapsAny(read, chain.scratch, chain.scratchCount))
            return false;
    }
    return true;
}

namespace pass_hazard_detail {

// `image` into `list` unless an image of the same memory is there; false where the list is full
inline bool addOnce(HazardImage *list, uint8_t &count, const HazardImage &image)
{
    for (uint8_t j = 0; j < count; j++)
        if (list[j].va == image.va && list[j].bytes == image.bytes && list[j].importBase == image.importBase)
            return true;
    if (count == kChainImages)
        return false;
    list[count++] = image;
    return true;
}

}  // namespace pass_hazard_detail

// The chain after `next` opened: itself alone after a drain, else added to it
inline void advanceChain(PassChain &chain, const PassUses &next, bool drained)
{
    using pass_hazard_detail::addOnce;
    if (drained)
        chain.reset();
    chain.valid = next.known && (drained || chain.valid);
    chain.depth = chain.depth || next.depthStencil;
    for (uint8_t w = 0; w < next.writtenCount && chain.valid; w++)
        chain.valid = addOnce(chain.written, chain.writtenCount, next.written[w]);
    for (uint8_t s = 0; s < next.sampledCount && chain.valid; s++)
        chain.valid = addOnce(chain.sampled, chain.sampledCount, next.sampled[s]);
    for (uint8_t k = 0; k < next.scratchCount && chain.valid; k++)
        chain.valid = addOnce(chain.scratch, chain.scratchCount, next.scratch[k]);
}

}  // namespace gm20b
