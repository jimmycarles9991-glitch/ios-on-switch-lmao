// SPDX-License-Identifier: MIT
// See multisample.h and Device::createStoredMultisample, createOwnedZeta (gpu.h).
#include "lock.h"
#include <algorithm>
#include "gpu.h"
#include "multisample.h"

namespace gm20b {

bool Device::createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                                     const ArenaAt *at, uint32_t compressKind)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Multisample;
    ask.compressKind = compressKind;
    ask.width = width;
    ask.height = height;
    ask.samples = samples;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

namespace {

// The layers storedZetaKind weighs for a zeta: a Depth32Float 2D array's as one while HW Test's
// switchMetalProbeZetaLayers: is on
uint32_t kindLayers([[maybe_unused]] const Device &d, [[maybe_unused]] uint32_t format, [[maybe_unused]] uint8_t type,
                    uint32_t layers)
{
#if SWITCH_METAL_DIAGNOSTICS
    if (d.probeZetaLayers.load(std::memory_order_relaxed) && format == 252 && type == fx::kTexTwoDArray)
        return 1;
#endif
    return layers;
}

}  // namespace

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled,
                             Texture &t, uint32_t layers, uint8_t type, uint32_t levels)
{
    uint64_t bytes = 0, align = 0;
    fx::MipChain chain;
    if (!zetaImageLayout(width, height, format, samples, layers, levels, type, sampled, t, chain, bytes, align))
        return false;
    const uint32_t z = zetaFormatIndex(format);
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (const uint32_t kind = levels == 1 ? storedZetaKind((k_.info().flags & TegraGM20BInfoCompression) != 0, format,
                                                           samples, kindLayers(*this, format, type, layers))
                                          : 0;
        kind && createCompressedZeta(bytes, kind, t))
        return true;
    Span span;
    if (!tiledAlloc(l, zChunks_[z], kZetaFormatInfo[z].kind, bytes, std::max(align, kBlAlign), span))
        return false;
    // made here whatever `sampled` says: recording threads read t.tic unlocked, so nothing may set it later
    if (!(t.tic = allocTic())) {
        tiledFree(zChunks_[z], span, deferred);
        return false;
    }
    t.va = span.va;
    t.storage = {span.va, bytes, false};
    ownedZeta_[span.va] = {z, span};
    if (levels > 1) {
        t.chain = new Texture::Chain;
        t.chain->layout = chain;
        t.chain->va = t.va;
        uint32_t w[8];
        fx::mipChainTic(chain, t.va, kZetaFormatInfo[z].tic0, false, 0, levels, w, type);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        noteTicWrite();
    } else {
        writeTic(t);
    }
    return true;
}

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                             const ArenaAt *at)
{
    if (samples < 2 && !at)
        return false;
    TextureAsk ask;
    ask.kind = TextureAsk::Zeta;
    ask.width = width;
    ask.height = height;
    ask.samples = samples;
    ask.format = format;
    ask.sampled = true;
    return createLaidOut(ask, t, at);
}

}  // namespace gm20b
