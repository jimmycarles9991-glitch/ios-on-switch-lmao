// SPDX-License-Identifier: MIT
// Rule barrier.pass_pixel: a pass opens with PIXEL_SHADER_BARRIER and its pixel-ordered texture invalidate (0x0f74), in
// place of today's drain, where every dependency it has on the passes since the last drain is a cell
// gpu.x1.pass_pixel_barrier held in: claim PASS on boots 8, 14, 15, 19 and 21, boot 21 22 cells held, none failed, 91,294
// passes (kHeldCells, that line's pixel_cells). A cell is a format, a layout, the read's shape and a size; a pass shows
// all of those but the shape of its samples, which only its program's arithmetic says. So a read of an earlier pass's
// target is held where every shape was held at its size (64x64 block-linear), or where the pass draws a pyramid level of
// it (a target at most half its size), or a write over it (waw); every other dependency, the 8 blind cells' sizes among
// them, and anything a pass does that no cell did (storage writes, depth, more than one sample, level or layer,
// vertex-stage samples, an unnamed or fenced read, a target sampled before it is written) keeps the drain.
#pragma once
#include <cstdint>
#include "raw_attachment_lifetime.h"

namespace raw {

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
    i.plain =
        t.va && !t.memoryless && t.samples == 1 && t.layers == 1 && t.depth == 1 && !t.chain && !t.zeta && !t.planar;
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

constexpr uint32_t kPassImages = 16;

// What a pass reads and writes. `known`: everything it reads came through its bindings and named uses (no heap, fence
// wait, storage write or an image past the lists); `vertexSamples`: its vertex stage binds a texture
struct PassUses {
    bool known = false, vertexSamples = false;
    uint32_t areaWidth = 0, areaHeight = 0;
    HazardImage written[kPassImages];  // its colour targets
    uint8_t writtenCount = 0;
    bool depthStencil = false;  // it binds a depth or stencil attachment
    HazardImage depth;
    HazardImage sampled[kPassImages];  // what its stages sample and the resources it names (useResource)
    bool named[kPassImages] = {};      // through a use, whose stage it does not say
    uint8_t sampledCount = 0;

    bool write(const HazardImage &image)
    {
        if (writtenCount == kPassImages)
            return known = false;
        written[writtenCount++] = image;
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

// The passes since the last drain: the images they wrote and sampled; `valid` false where one of them was not known
struct PassChain {
    bool valid = false;
    HazardImage written[2 * kPassImages], sampled[2 * kPassImages];
    uint8_t writtenCount = 0, sampledCount = 0;
    bool depth = false;  // one of them bound a depth or stencil attachment
};

// Whether `next`'s dependencies on the chain are all held cells: PIXEL_SHADER_BARRIER may open it
constexpr bool pixelBarrierHolds(const PassChain &chain, const PassUses &next)
{
    if (!chain.valid || !next.known || next.depthStencil || chain.depth)
        return false;
    for (uint8_t w = 0; w < next.writtenCount; w++) {
        const HazardImage &target = next.written[w];
        for (uint8_t j = 0; j < chain.sampledCount; j++)
            if (overlaps(target, chain.sampled[j]))
                return false;
        for (uint8_t j = 0; j < chain.writtenCount; j++)
            if (overlaps(target, chain.written[j]) && !(heldWrite(target) && heldWrite(chain.written[j])))
                return false;
    }
    for (uint8_t s = 0; s < next.sampledCount; s++) {
        const HazardImage &read = next.sampled[s];
        for (uint8_t j = 0; j < chain.writtenCount; j++) {
            if (!overlaps(read, chain.written[j]))
                continue;
            if (next.named[s] || next.vertexSamples || !heldRead(read, next.areaWidth, next.areaHeight) ||
                !heldRead(chain.written[j], next.areaWidth, next.areaHeight))
                return false;
        }
    }
    return true;
}

// The chain after `next` opened: itself alone after a drain, else added to it
inline void advanceChain(PassChain &chain, const PassUses &next, bool drained)
{
    if (drained)
        chain = PassChain{};
    chain.valid = next.known && (drained || chain.valid);
    chain.depth = chain.depth || next.depthStencil;
    for (uint8_t w = 0; w < next.writtenCount && chain.valid; w++) {
        if (chain.writtenCount == 2 * kPassImages)
            chain.valid = false;
        else
            chain.written[chain.writtenCount++] = next.written[w];
    }
    for (uint8_t s = 0; s < next.sampledCount && chain.valid; s++) {
        if (chain.sampledCount == 2 * kPassImages)
            chain.valid = false;
        else
            chain.sampled[chain.sampledCount++] = next.sampled[s];
    }
}

}  // namespace raw
