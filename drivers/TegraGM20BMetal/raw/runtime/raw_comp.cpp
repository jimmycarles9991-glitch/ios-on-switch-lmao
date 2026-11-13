// SPDX-License-Identifier: MIT
// Compressed memory for the raw path's memoryless attachments (raw_comp.h)
#include "raw_lock.h"
#include "raw_comp.h"

#include <os/log.h>
#include <pthread.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include "../fx/fx_image.h"
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

// A memoryless attachment's backing layout as Device::backing makes it: the zeta's, or one block-linear level of colour at
// its sample count (plainBacking's)
bool backingLayout(const Texture &m, Texture &t, fx::MipChain &c, uint64_t &bytes)
{
    t = Texture();
    if (m.zeta) {
        uint64_t align = 0;
        return zetaShape(m.width, m.height, m.format, m.samples, t, bytes, align);
    }
    const FormatInfo *f = formatInfo(m.format);
    if (!f || !(m.samples > 1 ? fx::msLayout(m.width, m.height, m.samples, m.bpp, c)
                              : fx::mipChainLayout(m.width, m.height, 1, m.bpp, c)))
        return false;
    t.width = m.width;
    t.height = m.height;
    t.format = m.format;
    t.bpp = m.bpp;
    t.samples = m.samples;
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

uint32_t Device::compKind(const Texture &m, bool blockLinear, bool copied) const
{
    bool zetaProbe = false;
#if SWITCH_RAW_DIAGNOSTICS
    zetaProbe = probeZetaKinds.load(std::memory_order_relaxed);
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probePlainMemory.load(std::memory_order_relaxed))
        return 0;
#endif
    return backingKind((k_.info().flags & TegraGM20BInfoCompression) != 0, blockLinear, m.zeta ? m.format : 0, m.bpp,
                       std::max<uint32_t>(m.samples, 1), copied, zetaProbe);
}

void Device::compAsk(const CompWant &w)
{
    if (comp_.asked(w, commits()))
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
            std::unique_lock<raw::Mutex> l(lock_);
            const auto asked = [&] {
                return stopping_ || !comp_.wanted.empty() || compRearm_ || zbc_.pending();
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
                zbcMake(l);
                trimBackings();
            }
        });
    compWake_.notify_one();
}

void Device::compPrearm(const Texture &m)
{
    if (m.samples < 2)
        return;
    std::lock_guard<raw::Mutex> l(lock_);
    const uint32_t kind = compKind(m, kMemorylessTag, true);
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0;
    if (!kind || !backingLayout(m, t, c, bytes))
        return;
    const uint64_t need = compBytes(bytes);
    if (comp_.fit(need, kind) == comp_.ready.end())
        compAsk({compClassBytes(need), kind});
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
            RAW_DIAGNOSTIC(stats.compMisses++);
            return nullptr;
        }
        comp_.mems.push_back(std::make_unique<CompMem>(CompMem{it->bo, kind, tag, 0}));
        mem = comp_.mems.back().get();
        comp_.ready.erase(it);
        backingsChanged();
    }
    t.va = mem->bo.va;
    t.storage = {t.va, bytes, false};
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
    RAW_DIAGNOSTIC(stats.compViews++);
#if SWITCH_RAW_DIAGNOSTICS
    if (!comp_.loggedOn) {
        comp_.loggedOn = true;
        os_log(compLog(), "raw: compression on: first backing %ux%u format %u kind 0x%x (%llu KiB)", t.width, t.height,
               t.format, kind, (unsigned long long)(mem->bo.bytes >> 10));
    }
#endif
    return b;
}

void Device::compMake(std::unique_lock<raw::Mutex> &l)
{
    while (!comp_.wanted.empty() && !stopping_) {
        const CompWant w = comp_.wanted.front();
        Bo bo;
        l.unlock();
#if SWITCH_RAW_DIAGNOSTICS
        const auto start = std::chrono::steady_clock::now();
#endif
        int kr = k_.allocCompressible(w.bytes, bo);
        const bool made = kr == KERN_SUCCESS;
#if SWITCH_RAW_DIAGNOSTICS
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
#endif
        l.lock();
        comp_.wanted.erase(std::find(comp_.wanted.begin(), comp_.wanted.end(), w));
        if (made && (kr = k_.bindCompressed(bo, w.kind)) == KERN_SUCCESS) {
            set_.add(k_, bo, TegraGM20BSurfaceWrite);
            comp_.made(w);
            comp_.ready.push_back({bo, w.kind, commits(), compClock()});
            backingsChanged();
            RAW_DIAGNOSTIC(stats.compMade++);
            RAW_DIAGNOSTIC(stats.compHeld++);
            RAW_DIAGNOSTIC(stats.compHeldBytes += bo.bytes);
            RAW_DIAGNOSTIC(glassMemEvent("compressed memory made", bo.bytes));
            RAW_DIAGNOSTIC(os_log_debug(compLog(), "raw: compressed memory %llu KiB kind 0x%x made in %lld us",
                                        (unsigned long long)(w.bytes >> 10), w.kind, (long long)us));
            continue;
        }
        if (made)
            k_.free(bo);
        RAW_DIAGNOSTIC(stats.compFailed++);
        const bool lasting = compLasting(kr);
        if (comp_.refuse(w, commits(), lasting))
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
    if (b.texture.tic)
        freeTic(b.texture.tic);
    RAW_DIAGNOSTIC(stats.compViews--);
    CompMem *mem = b.comp;
    b.comp = nullptr;
    if (--mem->views)
        return;
    RAW_DIAGNOSTIC(stats.compHeld--);
    RAW_DIAGNOSTIC(stats.compHeldBytes -= mem->bo.bytes);
    RAW_DIAGNOSTIC(glassMemEvent("compressed memory freed", mem->bo.bytes));
    k_.free(mem->bo);
    comp_.mems.erase(std::find_if(comp_.mems.begin(), comp_.mems.end(), [&](const std::unique_ptr<CompMem> &m) {
        return m.get() == mem;
    }));
    comp_.freed(commits());
}

uint64_t Device::compDeadline() const
{
    uint64_t at = 0;
    const auto due = [&](uint64_t ns) {
        at = at ? std::min(at, ns) : ns;
    };
    for (const Backing *b : backings_)
        if (b->retired)
            due(b->usedNs.load(std::memory_order_relaxed));  // once its last use completed
        else if (b->comp || b->plain)
            due(b->usedNs.load(std::memory_order_relaxed) + kCompIdleNs);
    for (const CompBo &r : comp_.ready)
        due(r.readyNs + kCompIdleNs);
    return at ? std::max(at, compClock() + kCompRecheckNs) : 0;
}

void Device::compTrim(uint64_t now)
{
    for (auto it = comp_.ready.begin(); it != comp_.ready.end();) {
        if (readyStale(commits(), it->madeAt, now, it->readyNs)) {
            RAW_DIAGNOSTIC(stats.compHeld--);
            RAW_DIAGNOSTIC(stats.compHeldBytes -= it->bo.bytes);
            RAW_DIAGNOSTIC(glassMemEvent("compressed memory freed unused", it->bo.bytes));
            k_.free(it->bo);
            it = comp_.ready.erase(it);
            comp_.freed(commits());
            continue;
        }
        ++it;
    }
}

int Kernel::zbcAdd(const TegraGM20BAbiZbc &entry)
{
    TegraGM20BAbiZbcReply r = {};
    return call(TegraGM20BMethodZbc, &entry, sizeof entry, &r, sizeof r);
}

bool Device::zbcHeld(const TegraGM20BAbiZbc &e)
{
    bool wanted = false;
    const bool held = zbc_.lookup(e, wanted);
    if (wanted) {
        std::lock_guard<raw::Mutex> l(lock_);
        compStart();
    }
    return held;
}

void Device::clearZbc(const Texture &t, bool full, const uint32_t v[4])
{
    if (!t.comptags)
        return;
    bool held = zbcBootColor(t.format, v);
    TegraGM20BAbiZbc e;
    if (!held && full) {
        if (zbcColorEntry(t.format, v, e))
            held = zbcHeld(e);
        else
            RAW_DIAGNOSTIC(stats.zbcInexact++);
    }
    RAW_DIAGNOSTIC((full && held ? stats.zbcHits : stats.zbcMisses)++);
}

void Device::clearZbcDepth(const Texture &t, bool full, float depth)
{
    if (!t.comptags)
        return;
    bool held = zbcBootDepth(depth);
    TegraGM20BAbiZbc e;
    if (!held && full) {
        if (zbcDepthEntry(t.format, depth, e))
            held = zbcHeld(e);
        else
            RAW_DIAGNOSTIC(stats.zbcInexact++);
    }
    RAW_DIAGNOSTIC((full && held ? stats.zbcHits : stats.zbcMisses)++);
}

void Device::zbcMake(std::unique_lock<raw::Mutex> &l)
{
    TegraGM20BAbiZbc e;
    while (!stopping_ && zbc_.next(e)) {
        l.unlock();
        const int kr = k_.zbcAdd(e);
        l.lock();
        zbc_.answer(e, kr == KERN_SUCCESS);
        RAW_DIAGNOSTIC((kr == KERN_SUCCESS ? stats.zbcAdded : stats.zbcFull)++);
        if (kr != KERN_SUCCESS && kr != kIOReturnNoResources)
            os_log_error(compLog(), "raw: ZBC entry format 0x%x not added (0x%x): those clears write their texels",
                         e.format, unsigned(kr));
    }
}

}  // namespace raw
