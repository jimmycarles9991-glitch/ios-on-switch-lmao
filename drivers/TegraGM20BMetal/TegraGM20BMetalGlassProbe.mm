// SPDX-License-Identifier: MIT
// DEVELOPMENT: the switches HW Test's glass tests (hw_test_app app27/GlassProbe.swift) post to every process's raw
// path, and the render server's record of its command buffers they read back.
// Each name below follows com.brunostjohn.switch.debug.:
//   glass-probe[-off]           the draw lines (glass_probe.h) and the record below
//   glass-record                the record alone (no lines to slow a moving scene)
//   split-capture[-off]         the next kSplitCaptureDraws split draws logged for the bench (split_capture.h) as
//                               `SwitchMetal: splitcap` lines, for kSplitCaptureMs at most
// glass-probe and glass-record also arm the blur-texture probe (glass_mem.h) with every command buffer's line, and
// glass-probe-off disarms it; glass-mem-watch[-off] arms it (or not) for kGlassMemWatchMs of normal use, with only the
// lines that found something. While armed this file publishes its counts and the pool's shape (glass-mem,
// glass-mem.pool) and, at each arming, until when (glass-mem.armed).
// The record: each command buffer the render server commits for kRecordSeconds after either arms it, as its API made it
// (draws, encoders, passes into a mip level above 0, the commit's mach time), in notify state, which crosses HW Test's
// sandbox where a file would not: glass-cb holds generation << 32 | count, glass-cb.<count % kRecordSlots> the latest
// entries (gm20b::GlassCbEntry). Either arming starts the next generation, which the render server's draw lines carry
// too.
#import "TegraGM20BMetalInternal.h"

#include <notify.h>
#include <mutex>
#include "glass_probe.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
namespace {

constexpr uint64_t kRecordSeconds = 20;
constexpr uint32_t kRecordSlots = 16;
constexpr const char *kRecordName = "com.brunostjohn.switch.debug.glass-cb";

struct Record {
    gm20b::Mutex lock;
    std::atomic<bool> armed{false};
    std::atomic<uint64_t> until{0};
    uint32_t generation = 0, count = 0;
    int countToken = -1;
    int slotTokens[kRecordSlots];
};

Record &record()
{
    static Record *r = new Record;
    return *r;
}

void armRecord(bool armed)
{
    Record &r = record();
    std::lock_guard<gm20b::Mutex> l(r.lock);
    if (r.countToken < 0) {
        if (notify_register_check(kRecordName, &r.countToken) != NOTIFY_STATUS_OK)
            return;
        for (uint32_t i = 0; i < kRecordSlots; i++) {
            char name[96];
            snprintf(name, sizeof name, "%s.%u", kRecordName, i);
            notify_register_check(name, &r.slotTokens[i]);
        }
    }
    r.armed.store(armed, std::memory_order_relaxed);
    if (armed) {
        r.until.store(mach_absolute_time() + gm20b::glassProbeTicks(kRecordSeconds * 1000), std::memory_order_relaxed);
        r.generation = gm20b::gGlassProbeGeneration.load(std::memory_order_relaxed);
        r.count = 0;
    }
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
}

// Until when the blur-texture probe is armed (mach time), its top bit set while HW Test's glass test armed it: the main
// screen's countdown and the frame-cost tests' refusal read it (hw_test_app kit/GlassMemWatch.swift)
void publishArming(gm20b::Device *dev)
{
    static int token = -1;
    static std::once_flag registered;
    std::call_once(registered, [] {
        notify_register_check("com.brunostjohn.switch.debug.glass-mem.armed", &token);
    });
    const uint64_t now = mach_absolute_time(), until = dev->glassMemArming.until();
    if (token >= 0)
        notify_set_state(token,
                         until > now ? (until & ~(1ull << 63)) | uint64_t(dev->glassMemArming.every(now)) << 63 : 0);
}

// The blur-texture probe armed for `ms` by HW Test's glass tests (`test`: every command buffer's line) or by the main
// screen's watch, and back on where a killed submission had stopped it
void armGlassMem(gm20b::Device *dev, bool test, uint64_t ms)
{
    if (!dev->glassMem)
        return;
    dev->glassMemArming.arm(test, mach_absolute_time() + gm20b::glassProbeTicks(ms));
    dev->glassMemStopped.store(false, std::memory_order_relaxed);
    dev->glassMemOn.store(true, std::memory_order_relaxed);
    publishArming(dev);
    os_log(gm20bLog(), "SwitchMetal: glassmem armed by %{public}s for %llu s",
           test ? "HW Test" : "the main screen's watch", (unsigned long long)(ms / 1000));
}

void disarmGlassMem(gm20b::Device *dev, bool test)
{
    if (!dev->glassMem)
        return;
    dev->glassMemArming.disarm(test);
    publishArming(dev);
}

// The probe's counts and the pool's shape in notify state for HW Test (ui.glass_sidebar): each commit while HW Test
// armed it, else once a second while armed; nothing while disarmed
void publishGlassMem(gm20b::Device &d)
{
    static std::atomic<uint64_t> last{0};
    static int memToken = -1, poolToken = -1;
    static std::once_flag registered;
    if (!d.glassMemOn.load(std::memory_order_relaxed))
        return;
    const uint64_t now = mach_absolute_time();
    if (!d.glassMemArming.every(now) && now - last.load(std::memory_order_relaxed) < gm20b::glassProbeTicks(1000))
        return;
    last.store(now, std::memory_order_relaxed);
    std::call_once(registered, [] {
        notify_register_check("com.brunostjohn.switch.debug.glass-mem", &memToken);
        notify_register_check("com.brunostjohn.switch.debug.glass-mem.pool", &poolToken);
    });
    const gm20b::Device::Stats &s = d.stats;
    if (memToken >= 0)
        notify_set_state(memToken, gm20b::glassMemWord(d.glassMem->counts));
    if (poolToken >= 0)
        notify_set_state(poolToken,
                         gm20b::glassMemPoolWord(s.poolBytes.load() >> 20, s.poolChunks.load(), s.backings.load(),
                                                 s.backingBytes.load() >> 20, s.compHeld.load(), s.compMade.load()));
}

}  // namespace

void splitCaptureCommitted(gm20b::CommandBuffer &rc)
{
    static std::atomic<uint32_t> serial{0};
    for (const gm20b::SplitCaptureRecord &r : rc.takeSplitCaptures()) {
        const uint32_t n = serial.fetch_add(1, std::memory_order_relaxed);
        for (const std::string &line : gm20b::splitCaptureLines(r, n))
            os_log(gm20bLog(), "SwitchMetal: %{public}s", line.c_str());
    }
}

static void watchSplitCapture(gm20b::Device *dev)
{
    static std::atomic<uint32_t> generation{0};
    for (const bool on : {true, false}) {
        int token;
        notify_register_dispatch(
            on ? "com.brunostjohn.switch.debug.split-capture" : "com.brunostjohn.switch.debug.split-capture-off",
            &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                dev->splitCaptureLeft.store(on ? gm20b::kSplitCaptureDraws : 0, std::memory_order_relaxed);
                const uint32_t armed = generation.fetch_add(1, std::memory_order_relaxed) + 1;
                os_log(gm20bLog(), "SwitchMetal: split capture %{public}s (debug notification)", on ? "armed" : "off");
                if (!on)
                    return;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, gm20b::kSplitCaptureMs * NSEC_PER_MSEC),
                               dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                                   if (generation.load(std::memory_order_relaxed) == armed)
                                       dev->splitCaptureLeft.store(0, std::memory_order_relaxed);
                               });
            });
    }
}

void watchGlassProbe(gm20b::Device *dev)
{
    watchSplitCapture(dev);
    const bool renderServer = dev->role() == gm20b::Device::Role::RenderServer;
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 gm20b::armGlassProbe();
                                 if (renderServer)
                                     armRecord(true);
                                 armGlassMem(dev, true, gm20b::kGlassMemArmMs);
                                 os_log(gm20bLog(), "SwitchMetal: glass probe armed g%u (debug notification)",
                                        gm20b::gGlassProbeGeneration.load(std::memory_order_relaxed));
                             });
    if (renderServer)
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-record", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     gm20b::gGlassProbeGeneration.fetch_add(1, std::memory_order_relaxed);
                                     armRecord(true);
                                     armGlassMem(dev, true, gm20b::kGlassMemArmMs);
                                 });
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe-off", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 gm20b::disarmGlassProbe();
                                 if (renderServer)
                                     armRecord(false);
                                 disarmGlassMem(dev, true);
                             });
    if (renderServer) {
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-mem-watch", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     armGlassMem(dev, false, gm20b::kGlassMemWatchMs);
                                 });
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-mem-watch-off", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     disarmGlassMem(dev, false);
                                 });
    }
}

void glassProbeCommitted(SwitchMetalCommandBuffer *cb)
{
    publishGlassMem(*gRM->raw);
    Record &r = record();
    gm20b::GlassCbEntry e;
    e.ticks = mach_absolute_time();
    if (!r.armed.load(std::memory_order_relaxed) || e.ticks >= r.until.load(std::memory_order_relaxed))
        return;
    for (size_t at = 0; at < cb->_plan.size(); at++) {
        const Cmd c = cb->_plan.legacy(at);
        e.draws += isPlanDraw(c.op);
        e.encoders += c.op == OpRender || c.op == OpCompute || c.op == OpBlit;
        if (c.op != OpRender || c.x >= cb->_passes.size())
            continue;
        const SwitchMetalPass &pass = cb->_passes[c.x];
        bool level = false;
        for (const SwitchMetalPass::Color &a : pass.color)
            level |= a.texture && a.level;
        e.levels += level;
    }
    std::lock_guard<gm20b::Mutex> l(r.lock);
    if (!r.armed || r.countToken < 0)
        return;
    notify_set_state(r.slotTokens[r.count % kRecordSlots], e.packed());
    r.count++;
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
    os_log(gm20bLog(), "SwitchMetal: glass cb g%u #%u draws %u encoders %u levels %u (mach %llu)", r.generation,
           r.count, e.draws, e.encoders, e.levels, (unsigned long long)e.ticks);
}
#endif
