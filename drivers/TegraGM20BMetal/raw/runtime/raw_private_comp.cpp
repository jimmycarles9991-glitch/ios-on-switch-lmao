// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_private_comp.h"
#include <os/log.h>
#include "raw_gpu.h"

namespace raw {

bool Device::createCompressedTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t kind, Texture &t)
{
    tgm_layout bl;
    if (!shape(width, height, format, t) || !t.renderable || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl) ||
        compressedKind(t.bpp, 0) != kind)
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probePlainPrivate.load(std::memory_order_relaxed) || probePlainMemory.load(std::memory_order_relaxed))
        return false;
#endif
    const uint64_t need = compClassBytes(compBytes(bl.bytes));
    auto it = comp_.fit(need, kind);
    if (it == comp_.ready.end()) {
        compAsk({need, kind});
        return false;
    }
    if (!(t.tic = allocTic()))
        return false;
    const Bo bo = it->bo;
    comp_.ready.erase(it);
    t.va = bo.va;
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    t.storage = {t.va, bl.bytes, false};
    t.comptags = true;
    writeTic(t);
    privateComp_[&t] = {bo, 0};
    privateMade_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Device::privateCompressed(const Texture &t)
{
    std::lock_guard<raw::Mutex> l(lock_);
    auto it = privateComp_.find(&t);
    return it != privateComp_.end() && t.comptags;
}

bool Device::plainFromCompressed(Texture &t)
{
    tgm_layout bl;
    if (!privateCompressed(t) || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl))
        return false;
    Texture plain = t;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        if (!blAlloc(bl.bytes, bl.align, plain.span))
            return false;
        if (!(plain.tic = allocTic())) {
            Lease *none = nullptr;
            freeSpan(plain.span, Memory::Bl, none);
            return false;
        }
    }
    plain.va = plain.span.va;
    plain.cpu = plain.span.cpu;
    plain.storage = {};
    plain.comptags = false;
    uint32_t seq = 0;
    CommandBuffer *rc = begin();
    bool copied = rc != nullptr;
    if (copied) {
        BlitEncoder *b = rc->blit();
        b->copyTexels(t, 0, 0, 0, plain, 0, 0, 0, t.width, t.height);
        b->end();
        copied = rc->commit(seq);
        if (!copied)
            rc->abandon();
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!copied) {
        freeTic(plain.tic);
        Lease *none = nullptr;
        freeSpan(plain.span, Memory::Bl, none);
        os_log_error(OS_LOG_DEFAULT, "raw: a compressed %ux%u texture of format %u could not move to plain memory",
                     t.width, t.height, t.format);
        return false;
    }
    writeTic(plain);
    privateComp_[&t].staleTic = t.tic;
    t = plain;
    privateTransitions_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Device::releasePrivateCompressed(const Texture &t)
{
    auto it = privateComp_.find(&t);
    if (it == privateComp_.end())
        return;
    if (it->second.staleTic)
        freeTic(it->second.staleTic);
    RAW_DIAGNOSTIC(stats.compHeld--);
    RAW_DIAGNOSTIC(stats.compHeldBytes -= it->second.bo.bytes);
    k_.free(it->second.bo);
    privateComp_.erase(it);
    comp_.freed(commitCount_);
}

}  // namespace raw
