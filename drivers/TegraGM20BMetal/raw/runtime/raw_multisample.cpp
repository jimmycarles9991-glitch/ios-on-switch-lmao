// SPDX-License-Identifier: MIT
// See raw_multisample.h and Device::createStoredMultisample, createOwnedZeta (raw_gpu.h).
#include "raw_lock.h"
#include <algorithm>
#include "raw_gpu.h"
#include "raw_multisample.h"

namespace raw {

bool Device::createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                                     const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Multisample;
    ask.width = width;
    ask.height = height;
    ask.samples = samples;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled,
                             Texture &t, uint32_t layers)
{
    uint64_t bytes = 0, align = 0;
    if (!samples || !layers || layers > 2048 || (layers > 1 && samples > 1) ||
        !zetaShape(width, height, format, samples, t, bytes, align))
        return false;
    if (layers > 1) {
        t.layers = layers;
        bytes = (t.layerBytes * layers + align - 1) / align * align;
        sampled = false;
    }
    const uint32_t z = zetaFormatIndex(format);
    std::lock_guard<raw::Mutex> l(lock_);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (const uint32_t kind = samples == 1 && layers == 1 ? compressedKind(t.bpp, format) : 0;
        kind && probeCompressNext.exchange(false) && probeCompressibleZeta(bytes, kind, sampled, t))
        return true;
#endif
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
    if (t.tic)
        writeTic(t);
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
    bool owned;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        owned = ownedZeta_.count(t.va) != 0;
    }
    if (!owned || t.samples > 1) {
        *why = t.comptags ? "its memory is compressed, which the copy engine is not proven to read as zeta"
               : !owned   ? "it is in no zeta memory of its own"
                          : "it is multisampled";
        return false;
    }
    const uint32_t layers = std::max<uint32_t>(t.layers, 1);
    if (!createZetaOver(m, t.width, t.height, t.format, t.tic != 0, over, why, layers))
        return false;
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

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
bool Device::probeCompressibleZeta(uint64_t bytes, uint32_t kind, bool sampled, Texture &t)
{
    Bo bo;
    if (k_.allocCompressible(compBytes(bytes), bo) != KERN_SUCCESS)
        return false;
    if (k_.bindCompressed(bo, kind) != KERN_SUCCESS || (sampled && !(t.tic = allocTic()))) {
        k_.free(bo);
        return false;
    }
    t.va = bo.va;
    t.storage = {bo.va, bytes, false};
    t.comptags = true;
    probeCompressed[bo.va] = bo;
    if (t.tic)
        writeTic(t);
    return true;
}
#endif

}  // namespace raw
