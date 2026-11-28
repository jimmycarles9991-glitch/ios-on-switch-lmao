// SPDX-License-Identifier: MIT
// Maxwell GM20B 3D pass-to-pass hazard rule: after the previous pass flushes the tiled cache (TILED_CACHE_FLUSH),
// PIXEL_SHADER_BARRIER followed by INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED (0x0f74) orders ROP->FS reads and
// ROP->ROP writes of plain color targets regardless of format, dimensions or sample pattern. Full WAIT_FOR_IDLE is kept
// for cross-stage reads (VS/TS/GS, unnamed useResource), depth/stencil, WAR over earlier sampled textures, unknown
// passes (heaps, fence waits, storage/shader/buffer writes), non-plain targets (volatile pitch, memoryless, zeta,
// planar, multisample, layered/3D/mip-chain), and scratch-vs-real overlap.
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

constexpr uint32_t kHazardBgra8 = 80, kHazardRgba16f = 115;

// One image a pass reads or writes. `plain`: single-sample, single-level, single-layer, non-volatile, non-zeta,
// non-planar, non-memoryless color target.
struct HazardImage {
    uint64_t va = 0, bytes = 0;
    uint64_t importBase = 0, importBytes = 0;
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

constexpr bool heldRead(const HazardImage &read)
{
    return read.plain;
}

constexpr bool heldWrite(const HazardImage &written)
{
    return written.plain;
}

constexpr uint32_t kPassImages = 16, kChainImages = 128;

enum class DrainCause : uint8_t {
    None,
    Unknown,
    Chain,
    Depth,
    Read,
    Write,
    WriteSampled,
    Scratch
};

struct PassUses {
    bool known = false, vertexSamples = false;
    bool shaderWrites = false, bufferWrites = false;
    uint32_t areaWidth = 0, areaHeight = 0;
    HazardImage written[8];
    uint8_t writtenCount = 0;
    HazardImage scratch[8];
    uint8_t scratchCount = 0;
    bool depthStencil = false;
    HazardImage depth;
    HazardImage sampled[kPassImages];
    bool named[kPassImages] = {};
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

    void reset()
    {
        known = vertexSamples = shaderWrites = bufferWrites = depthStencil = false;
        areaWidth = areaHeight = 0;
        writtenCount = scratchCount = sampledCount = 0;
    }

    void copyFrom(const PassUses &src)
    {
        known = src.known;
        vertexSamples = src.vertexSamples;
        shaderWrites = src.shaderWrites;
        bufferWrites = src.bufferWrites;
        areaWidth = src.areaWidth;
        areaHeight = src.areaHeight;
        writtenCount = src.writtenCount;
        for (uint8_t i = 0; i < writtenCount; ++i)
            written[i] = src.written[i];
        scratchCount = src.scratchCount;
        for (uint8_t i = 0; i < scratchCount; ++i)
            scratch[i] = src.scratch[i];
        depthStencil = src.depthStencil;
        if (depthStencil)
            depth = src.depth;
        sampledCount = src.sampledCount;
        for (uint8_t i = 0; i < sampledCount; ++i) {
            sampled[i] = src.sampled[i];
            named[i] = src.named[i];
        }
    }
};

struct PassChain {
    bool valid = false;
    HazardImage written[kChainImages], sampled[kChainImages], scratch[kChainImages];
    uint8_t writtenCount = 0, sampledCount = 0, scratchCount = 0;
    bool depth = false;

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
            if (next.named[s] || next.vertexSamples || !heldRead(read) || !heldRead(chain.written[j]))
                return DrainCause::Read;
        }
    }
    return DrainCause::None;
}

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
