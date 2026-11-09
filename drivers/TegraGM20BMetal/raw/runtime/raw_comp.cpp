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
        return zetaShape(m.width, m.height, m.format, m.samples, t, bytes, align);
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

int Kernel::allocCompressible(uint64_t bytes, Bo &bo)
{
    TegraGM20BAbiBoAlloc a = {bytes, kCompAlignLog2, TegraGM20BBoCompressible};
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

int Kernel::bindCompressed(Bo &bo, uint32_t kind)
{
    const uint64_t va = allocVa(bo.bytes, kCompGranule);
    if (!va)
        return kIOReturnNoSpace;
    TegraGM20BAbiVmBind b = {};
    b.count = 1;
    b.ops[0] = {TegraGM20BBindMap, kind, bo.handle, 0, va, bo.bytes};
    const size_t in = offsetof(TegraGM20BAbiVmBind, ops) + sizeof(b.ops[0]);
    if (const int kr = call(TegraGM20BMethodVmBind, &b, in, nullptr, 0); kr != KERN_SUCCESS) {
        freeVa(va, bo.bytes);
        return kr;
    }
    bo.va = bo.vaBase = va;
    return KERN_SUCCESS;
}

uint32_t Device::compKind(const Texture &m, uint32_t tag, bool blockLinear) const
{
    if (m.zeta && m.format != kZetaFormat)  // ZF32_X24S8_2CSZV is D32S8's layout alone
        return 0;
    return backingKind((k_.info().flags & TegraGM20BInfoCompression) != 0, blockLinear, m.zeta, m.bpp, m.samples, tag,
                       kMemorylessTag);
}

void Device::compAsk(const CompWant &w)
{
    if (comp_.asked(w, commitCount_))
        return;
    comp_.wanted.push_back(w);
    compStart();
}

void Device::compStart()
{
    if (!compThread_.joinable())
        compThread_ = std::thread([this] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_setname_np("com.brunostjohn.switch.raw.comp");
            followers().join();  // it binds under lock_, which the committer's render thread waits for
            std::unique_lock<std::mutex> l(lock_);
            const auto asked = [&] {
                return stopping_ || !comp_.wanted.empty() || compRearm_;
            };
            for (;;) {
                // what a process that commits nothing stopped using goes when it turns stale, not at its next commit
                if (const uint64_t at = compDeadline())
                    compWake_.wait_until(l, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at)), asked);
                else
                    compWake_.wait(l, asked);
                if (stopping_)
                    return;
                compRearm_ = false;
                compMake(l);
                trimBackings();
            }
        });
    compWake_.notify_one();
}

Device::Backing *Device::compBacking(const Texture &m, uint32_t tag, uint32_t kind)
{
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0;
    if (!backingLayout(m, t, c, bytes))
        return nullptr;
    const uint64_t need = compBytes(bytes);
    CompMem *mem = comp_.largest(tag, kind);
    if (!mem || mem->bo.bytes < need) {
        auto it = comp_.fit(need, kind);
        if (it == comp_.ready.end()) {
            compAsk({compClassBytes(need), kind});
            return nullptr;
        }
        comp_.mems.push_back(std::make_unique<CompMem>(CompMem{it->bo, kind, tag, 0}));
        mem = comp_.mems.back().get();
        comp_.ready.erase(it);
    }
    t.va = mem->bo.va;
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
    b->comp = mem;
    b->usedNs = compClock();
    mem->views++;
    stats.compViews++;
    if (!comp_.loggedOn) {
        comp_.loggedOn = true;
        os_log(compLog(), "raw: compression on: first backing %ux%u format %u kind 0x%x (%llu KiB)", t.width, t.height,
               t.format, kind, (unsigned long long)(mem->bo.bytes >> 10));
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
        int kr = k_.allocCompressible(w.bytes, bo);
        const bool made = kr == KERN_SUCCESS;
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        l.lock();
        comp_.wanted.erase(std::find(comp_.wanted.begin(), comp_.wanted.end(), w));
        if (made && (kr = k_.bindCompressed(bo, w.kind)) == KERN_SUCCESS) {
            comp_.made(w);
            comp_.ready.push_back({bo, w.kind, commitCount_, compClock()});
            stats.compMade++;
            stats.compHeld++;
            stats.compHeldBytes += bo.bytes;
            os_log_debug(compLog(), "raw: compressed memory %llu KiB kind 0x%x made in %lld us",
                         (unsigned long long)(w.bytes >> 10), w.kind, (long long)us);
            continue;
        }
        if (made)
            k_.free(bo);
        stats.compFailed++;
        const bool lasting = compLasting(kr);
        if (comp_.refuse(w, commitCount_, lasting))
            os_log_error(compLog(),
                         "raw: compressed memory %llu KiB kind 0x%x not made (%{public}s, 0x%x): those backings stay "
                         "uncompressed%{public}s",
                         (unsigned long long)(w.bytes >> 10), w.kind,
                         !made                        ? "compressible BoAlloc"
                         : kr == kIOReturnNoResources ? "no comptags left"
                         : kr == kIOReturnNoSpace     ? "no GPU VA"
                                                      : "compressed VmBind",
                         unsigned(kr), lasting ? "" : " until it is made (asked again later)");
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
    stats.compViews--;
    CompMem *mem = b.comp;
    b.comp = nullptr;
    if (--mem->views)
        return;
    stats.compHeld--;
    stats.compHeldBytes -= mem->bo.bytes;
    k_.free(mem->bo);
    comp_.mems.erase(std::find_if(comp_.mems.begin(), comp_.mems.end(), [&](const std::unique_ptr<CompMem> &m) {
        return m.get() == mem;
    }));
    comp_.freed(commitCount_);
}

uint64_t Device::compDeadline() const
{
    uint64_t at = 0;
    const auto due = [&](uint64_t ns) {
        at = at ? std::min(at, ns) : ns;
    };
    for (const Backing *b : backings_)
        if (b->retired)
            due(b->usedNs);  // once its last use completed
        else if (b->comp || b->plain)
            due(b->usedNs + kCompIdleNs);
    for (const CompBo &r : comp_.ready)
        due(r.readyNs + kCompIdleNs);
    return at ? std::max(at, compClock() + kCompRecheckNs) : 0;
}

void Device::compTrim(uint64_t now)
{
    for (auto it = comp_.ready.begin(); it != comp_.ready.end();) {
        if (readyStale(commitCount_, it->madeAt, now, it->readyNs)) {
            stats.compHeld--;
            stats.compHeldBytes -= it->bo.bytes;
            k_.free(it->bo);
            it = comp_.ready.erase(it);
            comp_.freed(commitCount_);
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
