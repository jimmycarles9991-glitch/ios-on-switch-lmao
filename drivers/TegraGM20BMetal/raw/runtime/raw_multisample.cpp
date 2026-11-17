// SPDX-License-Identifier: MIT
// See raw_multisample.h and Device::createStoredMultisample, createOwnedZeta (raw_gpu.h).
#include "raw_lock.h"
#include <algorithm>
#include "raw_gpu.h"
#include "raw_multisample.h"

namespace raw {

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

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled,
                             Texture &t, uint32_t layers, uint8_t type, uint32_t levels)
{
    uint64_t bytes = 0, align = 0;
    fx::MipChain chain;
    if (!zetaImageLayout(width, height, format, samples, layers, levels, type, sampled, t, chain, bytes, align))
        return false;
    const uint32_t z = zetaFormatIndex(format);
    std::lock_guard<raw::Mutex> l(lock_);
    if (const uint32_t kind =
            levels == 1 ? storedZetaKind((k_.info().flags & TegraGM20BInfoCompression) != 0, format, samples, layers)
                        : 0;
        kind && createCompressedZeta(bytes, kind, sampled, t))
        return true;
    Span span;
    if (!tiledAlloc(zChunks_[z], kZetaFormatInfo[z].kind, bytes, std::max(align, kBlAlign), span))
        return false;
    if (sampled && !(t.tic = allocTic())) {
        tiledFree(zChunks_[z], span);
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
        if (t.tic)
            memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    } else if (t.tic) {
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

bool Device::moveOwnedZeta(Texture &t, const ZetaMemory &m, const char **why)
{
    Texture over;
    uint32_t layers;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        const bool owned = ownedZeta_.count(t.va) != 0 || compZeta_.count(t.va) != 0;
        if (!owned || t.samples > 1 || t.chain) {
            *why = !owned    ? "it is in no zeta memory of its own"
                   : t.chain ? "it is mipmapped (the copy moves level 0)"
                             : "it is multisampled";
            return false;
        }
        layers = std::max<uint32_t>(t.layers, 1);
        if (!createZetaOver(m, t.width, t.height, t.format, true, over, why, layers))
            return false;
    }
    uint32_t seqno = 0;
    CommandBuffer *rc = begin();
    bool copied = rc != nullptr;
    if (copied) {
        BlitEncoder *b = rc->blit();
        for (uint32_t layer = 0; layer < layers; layer++)
            b->copyTexels(t, 0, 0, layer, over, 0, 0, layer, t.width, t.height);
        b->end();
        copied = rc->commit(seqno);
        if (!copied)
            rc->abandon();
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!copied) {
        if (over.tic)
            freeTic(over.tic);
        *why = "the copy into NVK's image was not submitted";
        return false;
    }
    if (t.tic && layers > 1 && (over.tic = allocTic())) {
        over.type = t.type;
        writeTic(over);
    }
    movedZeta_[over.va] = {t.va, t.tic};
    t.va = over.va;
    t.storage = over.storage;
    t.pitch = over.pitch;
    t.blockHeight = over.blockHeight;
    t.layerBytes = over.layerBytes;
    t.comptags = over.comptags;
    t.tic = over.tic;
    return true;
}

}  // namespace raw
