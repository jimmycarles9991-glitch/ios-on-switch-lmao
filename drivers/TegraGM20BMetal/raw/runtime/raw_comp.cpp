// SPDX-License-Identifier: MIT
// Compressed memory for the raw path's memoryless attachments (raw_comp.h)
#include "raw_comp.h"

#include <os/log.h>
#include <pthread.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include "raw_follow.h"
#include "raw_gpu.h"
#include "raw_zeta.h"

namespace raw {
namespace {

os_log_t compLog()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    return log;
}

// A memoryless attachment's backing layout as Device::backing makes it: the zeta's, or one block-linear level of colour
bool backingLayout(const Texture &m, Texture &t, fx::MipChain &c, uint64_t &bytes)
{
    t = Texture();
    if (m.zeta) {
        uint64_t align = 0;
        return zetaShape(m.width, m.height, t, bytes, align);
    }
    const FormatInfo *f = formatInfo(m.format);
    if (!f || !fx::mipChainLayout(m.width, m.height, 1, m.bpp, c))
        return false;
    t.width = m.width;
    t.height = m.height;
    t.format = m.format;
    t.bpp = m.bpp;
    t.renderable = f->rt != 0;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
    bytes = c.bytes;
    return true;
}

}  // namespace

bool CompQueue::asked(const CompWant &w) const
{
    auto has = [&](const std::vector<CompWant> &v) {
        return std::find(v.begin(), v.end(), w) != v.end();
    };
    return has(wanted) || has(failed) || std::any_of(ready.begin(), ready.end(), [&](const CompBo &b) {
               return b.bo.bytes == w.bytes && b.kind == w.kind;
           });
}

int Kernel::allocContiguous(uint64_t bytes, uint32_t alignLog2, Bo &bo)
{
    TegraGM20BAbiBoAlloc a = {bytes, alignLog2, TegraGM20BBoContiguous};
    TegraGM20BAbiBoAllocReply r = {};
    bo = {};
    const int kr = call(TegraGM20BMethodBoAlloc, &a, sizeof a, &r, sizeof r);
    if (kr == KERN_SUCCESS) {
        bo.handle = r.handle;
        bo.token = r.map_token;
        bo.bytes = bytes;
    }
    return kr;
}

bool Kernel::bindCompressed(Bo &bo, uint32_t kind, bool &noLines)
{
    noLines = false;
    const uint64_t va = allocVa(bo.bytes, kCompGranule);
    if (!va)
        return false;
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {TegraGM20BBindMap, kind, bo.handle, 0, va, bo.bytes};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS) {
        freeVa(va, bo.bytes);
        noLines = kr == kIOReturnNoResources;
        return false;
    }
    bo.va = bo.vaBase = va;
    return true;
}

uint32_t Device::compKind(const Texture &m, uint32_t tag, bool blockLinear) const
{
    if (!(k_.info().flags & TegraGM20BInfoCompression) || !blockLinear)
        return 0;
    if (m.zeta)
        return kCompKindZeta;
    // shadows (tags below kMemorylessTag) are filled and drained by the copy engine: uncompressed
    return tag >= kMemorylessTag ? compressedKind(m.bpp, false) : 0;
}

Device::Backing *Device::compBacking(const Texture &m, uint32_t tag, uint32_t kind)
{
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0;
    if (!backingLayout(m, t, c, bytes))
        return nullptr;
    const CompWant want{compBytes(bytes), kind};
    auto ready = [&] {
        return std::find_if(comp_.ready.begin(), comp_.ready.end(), [&](const CompBo &b) {
            return b.bo.bytes == want.bytes && b.kind == want.kind;
        });
    };
    auto it = ready();
    if (it == comp_.ready.end()) {
        if (!comp_.asked(want)) {
            comp_.wanted.push_back(want);
            if (!compThread_.joinable())
                compThread_ = std::thread([this] {
                    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
                    pthread_setname_np("com.brunostjohn.switch.raw.comp");
                    followers().join();  // it binds under lock_, which the committer's render thread waits for
                    std::unique_lock<std::mutex> l(lock_);
                    for (;;) {
                        compWake_.wait(l, [&] {
                            return stopping_ || !comp_.wanted.empty();
                        });
                        if (stopping_)
                            return;
                        compMake(l);
                    }
                });
            compWake_.notify_one();
        }
        return nullptr;
    }
    t.va = it->bo.va;
    t.comptags = true;
    if (!m.zeta) {
        const FormatInfo *f = formatInfo(m.format);
        if (!(t.tic = allocTic()))
            return nullptr;
        uint32_t w[8];
        fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, 1, w);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    }
    auto *b = new Backing;
    b->texture = t;
    b->tag = tag;
    b->comp = it->bo;
    comp_.ready.erase(it);
    stats.compBackings++;
    stats.compBytes += b->comp.bytes;
    if (!comp_.loggedOn) {
        comp_.loggedOn = true;
        os_log(compLog(), "raw: compression on: first backing %ux%u format %u kind 0x%x (%llu KiB)", t.width, t.height,
               t.format, kind, (unsigned long long)(b->comp.bytes >> 10));
    }
    return b;
}

void Device::compMake(std::unique_lock<std::mutex> &l)
{
    while (!comp_.wanted.empty() && !stopping_) {
        const CompWant w = comp_.wanted.front();
        Bo bo;
        l.unlock();
        const auto start = std::chrono::steady_clock::now();
        const int kr = k_.allocContiguous(w.bytes, kCompAlignLog2, bo);
        const bool made = kr == KERN_SUCCESS;
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        l.lock();
        comp_.wanted.erase(std::find(comp_.wanted.begin(), comp_.wanted.end(), w));
        bool noLines = false;
        if (made && k_.bindCompressed(bo, w.kind, noLines)) {
            comp_.ready.push_back({bo, w.kind, commitCount_});
            os_log_debug(compLog(), "raw: compressed memory %llu KiB kind 0x%x made in %lld us",
                         (unsigned long long)(w.bytes >> 10), w.kind, (long long)us);
            continue;
        }
        if (made)
            k_.free(bo);
        comp_.failed.push_back(w);
        stats.compFailed++;
        os_log_error(compLog(),
                     "raw: compressed memory %llu KiB kind 0x%x not made (%{public}s, 0x%x): those backings stay "
                     "uncompressed",
                     (unsigned long long)(w.bytes >> 10), w.kind,
                     !made     ? "contiguous BoAlloc"
                     : noLines ? "no comptags left"
                               : "compressed VmBind",
                     unsigned(kr));
    }
}

void Device::compStop()
{
    compWake_.notify_all();
    if (compThread_.joinable())
        compThread_.join();
}

void Device::compFree(Backing &b)
{
    if (b.texture.tic) {
        memset(tic_.cpu + 32 * b.texture.tic, 0, 32);
        freeTic_.push_back(b.texture.tic);
    }
    k_.free(b.comp);
}

void Device::compTrim()
{
    for (auto it = comp_.ready.begin(); it != comp_.ready.end();) {
        if (commitCount_ - it->madeAt > 600) {
            k_.free(it->bo);
            it = comp_.ready.erase(it);
            continue;
        }
        ++it;
    }
}

void Device::countZbc(const Texture &t, bool full, bool boot)
{
    if (!t.comptags)
        return;
    (full && boot ? stats.zbcHits : stats.zbcMisses)++;
}

}  // namespace raw
