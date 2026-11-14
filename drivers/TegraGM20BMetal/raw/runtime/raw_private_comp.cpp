// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#include "raw_private_comp.h"
#include <os/log.h>
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"

namespace raw {
namespace {

// `from`'s texels into `to`, the same layout on other memory, by the copy engine: each level of a chain (level 0 the
// texture itself: chainLevelImage names the others), each layer, and a multisampled image as the grid of its samples
void copyImages(BlitEncoder &b, const Texture &from, const Texture &to)
{
    if (from.chain && to.chain) {
        b.copyTexels(from, 0, 0, 0, to, 0, 0, 0, from.width, from.height);
        for (uint32_t level = 1; level < from.chain->layout.levels; level++) {
            Texture a, c;
            if (chainLevelImage(from, level, a) && chainLevelImage(to, level, c))
                b.copyTexels(a, 0, 0, 0, c, 0, 0, 0, a.width, a.height);
        }
        return;
    }
    Texture a = from, c = to;
    uint32_t sx, sy;
    sampleGrid(from.samples, sx, sy);
    for (Texture *grid : {&a, &c}) {
        grid->width <<= sx;
        grid->height <<= sy;
        grid->samples = 1;
    }
    for (uint32_t layer = 0; layer < std::max(from.layers, 1u); layer++)
        b.copyTexels(a, 0, 0, layer, c, 0, 0, layer, a.width, a.height);
}

}  // namespace

bool Device::takeCompressed(uint64_t bytes, uint32_t kind, Bo &bo)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probePlainPrivate.load(std::memory_order_relaxed) || probePlainMemory.load(std::memory_order_relaxed))
        return false;
#endif
    // its own whole comptag lines: a private texture's memory is its alone and never grows, as a backing's class does
    const uint64_t need = compBytes(bytes);
    auto it = comp_.fit(need, kind);
    if (it == comp_.ready.end()) {
        compAsk({need, kind});
        return false;
    }
    bo = it->bo;
    comp_.ready.erase(it);
    return true;
}

void Device::noteCompressed(const TextureAsk &ask, const Bo &bo, uint64_t bytes, Texture &t)
{
    t.va = bo.va;
    t.cpu = nullptr;
    t.storage = {t.va, bytes, false};
    t.comptags = true;
    PrivateCompTexture &p = privateComp_[&t];
    p.comp = {bo};
    p.ask = ask;
    p.ask.compressKind = 0;
    privateMade_.store(privateMade_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

bool Device::createCompressedTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t kind, Texture &t)
{
    tgm_layout bl;
    if (!shape(width, height, format, t) || !t.renderable || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl) ||
        compressedKind(t.bpp, 0) != kind)
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    Bo bo;
    if (!takeCompressed(bl.bytes, kind, bo))
        return false;
    if (!(t.tic = allocTic())) {
        comp_.ready.push_back({bo, kind, commits(), compClock()});
        return false;
    }
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = true;
    noteCompressed(ask, bo, bl.bytes, t);
    writeTic(t);
    return true;
}

bool Device::createCompressedZeta(uint64_t bytes, uint32_t kind, bool sampled, Texture &t)
{
    Bo bo;
    if (!takeCompressed(bytes, kind, bo))
        return false;
    if (sampled && !(t.tic = allocTic())) {
        comp_.ready.push_back({bo, kind, commits(), compClock()});
        return false;
    }
    t.va = bo.va;
    t.storage = {bo.va, bytes, false};
    t.comptags = true;
    compZeta_[bo.va] = bo;
    if (t.tic)
        writeTic(t);
    privateMade_.store(privateMade_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

void Device::freeCompressedZeta(uint64_t va)
{
    const auto it = compZeta_.find(va);
    if (it == compZeta_.end())
        return;
    RAW_DIAGNOSTIC(stats.compHeld.store(stats.compHeld.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed));
    RAW_DIAGNOSTIC(stats.compHeldBytes.store(stats.compHeldBytes.load(std::memory_order_relaxed) - it->second.bytes,
                                             std::memory_order_relaxed));
    k_.free(it->second);
    compZeta_.erase(it);
    comp_.freed();
}

bool Device::privateCompressed(const Texture &t)
{
    std::lock_guard<raw::Mutex> l(lock_);
    auto it = privateComp_.find(&t);
    return it != privateComp_.end() && t.comptags;
}

bool Device::plainTiledTwin(const Texture &t, Texture &plain)
{
    tgm_layout bl;
    if (!tgm_layout_tiled(t.width, t.height, t.bpp, &bl))
        return false;
    plain = t;
    std::lock_guard<raw::Mutex> l(lock_);
    if (!blAlloc(bl.bytes, bl.align, plain.span))
        return false;
    if (!(plain.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(plain.span, Memory::Bl, none);
        return false;
    }
    plain.va = plain.span.va;
    plain.cpu = plain.span.cpu;
    plain.storage = {};
    plain.comptags = false;
    writeTic(plain);
    return true;
}

void Device::noteView(const Texture &of, Texture &view)
{
    if (!of.comptags)
        return;
    for (auto &[texture, p] : privateComp_)
        if (texture->comptags && of.va >= p.comp.bo.va && of.va - p.comp.bo.va < p.comp.bo.bytes) {
            p.views.push_back(&view);
            return;
        }
}

void Device::forgetView(const Texture &view)
{
    for (auto &[texture, p] : privateComp_)
        p.views.erase(std::remove(p.views.begin(), p.views.end(), &view), p.views.end());
}

bool Device::plainFromCompressed(Texture &t)
{
    TextureAsk ask;
    std::vector<Texture *> views;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        auto it = privateComp_.find(&t);
        if (it == privateComp_.end() || !t.comptags)
            return false;
        ask = it->second.ask;
        views = it->second.views;
    }
    Texture plain;
    if (ask.kind == TextureAsk::Plain ? !plainTiledTwin(t, plain) : !createLaidOut(ask, plain, nullptr))
        return false;
    // the texture's own header first, then its views': each as it is (swizzle, format, levels), naming the plain memory
    std::vector<uint32_t> tics{t.tic};
    for (const Texture *v : views)
        tics.push_back(v->tic);
    Buffer headers;
    if (!createBuffer(32 * tics.size(), headers, true)) {
        destroyTexture(plain);
        return false;
    }
    for (size_t i = 0; i < tics.size(); i++) {
        uint32_t w[8];
        memcpy(w, tic_.cpu + 32 * tics[i], 32);
        ticAt(w, ticAddress(w) - t.va + plain.va);
        memcpy(headers.cpu + 32 * i, w, 32);
    }
    uint32_t seq = 0;
    CommandBuffer *rc = begin();
    bool copied = rc != nullptr;
    if (copied) {
        BlitEncoder *b = rc->blit();
        copyImages(*b, t, plain);
        for (size_t i = 0; i < tics.size(); i++)
            b->copyBytes(headers.va + 32 * i, tic_.va + 32ull * tics[i], 32);
        b->end();
        copied = rc->commit(seq);
        if (!copied)
            rc->abandon();
    }
    if (!copied) {
        destroyTexture(plain);
        destroyBuffer(headers);
        os_log_error(OS_LOG_DEFAULT, "raw: a compressed %ux%u texture of format %u could not move to plain memory",
                     t.width, t.height, t.format);
        return false;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    PrivateCompTexture &p = privateComp_[&t];
    freeTic(plain.tic);
    p.staleChain = t.chain;
    p.headers = headers;
    const uint64_t from = t.va;
    t.va = plain.va;
    t.cpu = plain.cpu;
    t.span = plain.span;
    t.lease = plain.lease;
    t.storage = plain.storage;
    t.chain = plain.chain;
    t.comptags = false;
    for (Texture *v : p.views) {
        v->va = v->va - from + plain.va;
        v->cpu = plain.cpu ? plain.cpu + (v->va - plain.va) : nullptr;
        v->storage = t.storageRange();
        v->comptags = false;
    }
    p.views.clear();
    privateTransitions_.store(privateTransitions_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

void Device::releasePrivateCompressed(const Texture &t)
{
    auto it = privateComp_.find(&t);
    if (it == privateComp_.end())
        return;
    if (Buffer &headers = it->second.headers; headers.va)
        freeSpan(headers.span, headers.wc ? Memory::Wc : Memory::Pool, headers.lease);
    if (Texture::Chain *stale = it->second.staleChain) {
        for (const uint32_t level : stale->levelTic)
            if (level)
                freeTic(level);
        delete stale;
    }
    RAW_DIAGNOSTIC(stats.compHeld--);
    RAW_DIAGNOSTIC(stats.compHeldBytes -= it->second.comp.bo.bytes);
    k_.free(it->second.comp.bo);
    privateComp_.erase(it);
    comp_.freed();
}

}  // namespace raw
