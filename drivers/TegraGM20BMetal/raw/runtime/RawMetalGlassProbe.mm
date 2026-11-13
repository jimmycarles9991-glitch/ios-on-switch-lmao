// SPDX-License-Identifier: MIT
// DEVELOPMENT: the switches HW Test's glass tests (hw_test_app app27/GlassProbe.swift) post to every process's raw
// path, and the render server's record of its command buffers they read back.
// Each name below follows com.brunostjohn.switch.debug.:
//   glass-probe[-off]           the draw lines (raw_glass_probe.h) and the record below
//   glass-record                the record alone (no lines to slow a moving scene)
//   whole-programs-on|-off      every command buffer draws whole programs (switchRawWholePrograms)
//   zero-new-buffers-on|-off    every new buffer starts zeroed
//   plain-memory-on|-off        textures, backings and shadows made take no compressible memory (ui.glass's plain arm)
//   half-view-refuse-on|-off    a kernel writing a half-float texture through m2v's view goes to m2v, as before T-v2
//                               (ui.glass's halfview arm)
//   split-capture[-off]         the next kSplitCaptureDraws split draws logged for the bench (raw_split_capture.h) as
//                               `raw: splitcap` lines, for kSplitCaptureMs at most
// glass-probe and glass-record also arm the blur-texture probe (raw_glass_mem.h) with every command buffer's line, and
// glass-probe-off disarms it; glass-mem-watch[-off] arms it (or not) for kGlassMemWatchMs of normal use, with only the
// lines that found something. While armed this file publishes its counts and the pool's shape (glass-mem,
// glass-mem.pool) and, at each arming, until when (glass-mem.armed).
// A switch left on turns itself off after kForcedSeconds: a test that died with it on leaves no process forced.
// The record: each command buffer the render server commits for kRecordSeconds after either arms it, as its API made it
// (draws, encoders, passes into a mip level above 0, the commit's mach time), in notify state, which crosses HW Test's
// sandbox where a file would not: glass-cb holds generation << 32 | count, glass-cb.<count % kRecordSlots> the latest
// entries (raw::GlassCbEntry). Either arming starts the next generation, which the render server's draw lines carry
// too.
#import "RawMetalInternal.h"

#include <notify.h>
#include <mutex>
#include "raw_glass_probe.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
namespace {

constexpr int64_t kForcedSeconds = 60;
constexpr uint64_t kRecordSeconds = 20;
constexpr uint32_t kRecordSlots = 16;
constexpr const char *kRecordName = "com.brunostjohn.switch.debug.glass-cb";

struct Forced {
    std::atomic<bool> on{false};
    std::atomic<uint32_t> generation{0};
};

Forced gWholePrograms, gZeroNewBuffers, gPlainMemory, gHalfViewRefuse;

struct Record {
    raw::Mutex lock;
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

// `mirror`: a device flag that follows the switch, for the raw device's own decisions
void watchForced(Forced &f, const char *name, std::atomic<bool> *mirror = nullptr)
{
    for (const bool on : {true, false}) {
        char full[96];
        snprintf(full, sizeof full, "com.brunostjohn.switch.debug.%s-%s", name, on ? "on" : "off");
        int token;
        notify_register_dispatch(full, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            f.on.store(on, std::memory_order_relaxed);
            if (mirror)
                mirror->store(on, std::memory_order_relaxed);
            const uint32_t generation = f.generation.fetch_add(1, std::memory_order_relaxed) + 1;
            os_log(rawLog(), "raw: %{public}s %{public}s (debug notification)", name, on ? "on" : "off");
            if (!on)
                return;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kForcedSeconds * NSEC_PER_SEC),
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                               if (f.generation.load(std::memory_order_relaxed) != generation)
                                   return;
                               f.on.store(false, std::memory_order_relaxed);
                               if (mirror)
                                   mirror->store(false, std::memory_order_relaxed);
                               os_log(rawLog(), "raw: %{public}s off after %lld s", name, kForcedSeconds);
                           });
        });
    }
}

void armRecord(bool armed)
{
    Record &r = record();
    std::lock_guard<raw::Mutex> l(r.lock);
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
        r.until.store(mach_absolute_time() + raw::glassProbeTicks(kRecordSeconds * 1000), std::memory_order_relaxed);
        r.generation = raw::gGlassProbeGeneration.load(std::memory_order_relaxed);
        r.count = 0;
    }
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
}

// Until when the blur-texture probe is armed (mach time), its top bit set while HW Test's glass test armed it: the main
// screen's countdown and the frame-cost tests' refusal read it (hw_test_app kit/GlassMemWatch.swift)
void publishArming(raw::Device *dev)
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
void armGlassMem(raw::Device *dev, bool test, uint64_t ms)
{
    if (!dev->glassMem)
        return;
    dev->glassMemArming.arm(test, mach_absolute_time() + raw::glassProbeTicks(ms));
    dev->glassMemStopped.store(false, std::memory_order_relaxed);
    dev->glassMemOn.store(true, std::memory_order_relaxed);
    publishArming(dev);
    os_log(rawLog(), "raw: glassmem armed by %{public}s for %llu s", test ? "HW Test" : "the main screen's watch",
           (unsigned long long)(ms / 1000));
}

void disarmGlassMem(raw::Device *dev, bool test)
{
    if (!dev->glassMem)
        return;
    dev->glassMemArming.disarm(test);
    publishArming(dev);
}

// The probe's counts and the pool's shape in notify state for HW Test (ui.glass_sidebar): each commit while HW Test
// armed it, else once a second while armed; nothing while disarmed
void publishGlassMem(raw::Device &d)
{
    static std::atomic<uint64_t> last{0};
    static int memToken = -1, poolToken = -1;
    static std::once_flag registered;
    if (!d.glassMemOn.load(std::memory_order_relaxed))
        return;
    const uint64_t now = mach_absolute_time();
    if (!d.glassMemArming.every(now) && now - last.load(std::memory_order_relaxed) < raw::glassProbeTicks(1000))
        return;
    last.store(now, std::memory_order_relaxed);
    std::call_once(registered, [] {
        notify_register_check("com.brunostjohn.switch.debug.glass-mem", &memToken);
        notify_register_check("com.brunostjohn.switch.debug.glass-mem.pool", &poolToken);
    });
    const raw::Device::Stats &s = d.stats;
    if (memToken >= 0)
        notify_set_state(memToken, raw::glassMemWord(d.glassMem->counts));
    if (poolToken >= 0)
        notify_set_state(poolToken,
                         raw::glassMemPoolWord(s.poolBytes.load() >> 20, s.poolChunks.load(), s.backings.load(),
                                               s.backingBytes.load() >> 20, s.compHeld.load(), s.compMade.load()));
}

}  // namespace

bool rmWholePrograms()
{
    return gWholePrograms.on.load(std::memory_order_relaxed);
}

bool rmHalfViewRefused()
{
    return gHalfViewRefuse.on.load(std::memory_order_relaxed);
}

bool rmZeroNewBuffers()
{
    return gZeroNewBuffers.on.load(std::memory_order_relaxed);
}

void splitCaptureCommitted(raw::CommandBuffer &rc)
{
    static std::atomic<uint32_t> serial{0};
    for (const raw::SplitCaptureRecord &r : rc.takeSplitCaptures()) {
        const uint32_t n = serial.fetch_add(1, std::memory_order_relaxed);
        for (const std::string &line : raw::splitCaptureLines(r, n))
            os_log(rawLog(), "raw: %{public}s", line.c_str());
    }
}

static void watchSplitCapture(raw::Device *dev)
{
    static std::atomic<uint32_t> generation{0};
    for (const bool on : {true, false}) {
        int token;
        notify_register_dispatch(
            on ? "com.brunostjohn.switch.debug.split-capture" : "com.brunostjohn.switch.debug.split-capture-off",
            &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                dev->splitCaptureLeft.store(on ? raw::kSplitCaptureDraws : 0, std::memory_order_relaxed);
                const uint32_t armed = generation.fetch_add(1, std::memory_order_relaxed) + 1;
                os_log(rawLog(), "raw: split capture %{public}s (debug notification)", on ? "armed" : "off");
                if (!on)
                    return;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, raw::kSplitCaptureMs * NSEC_PER_MSEC),
                               dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                                   if (generation.load(std::memory_order_relaxed) == armed)
                                       dev->splitCaptureLeft.store(0, std::memory_order_relaxed);
                               });
            });
    }
}

void watchGlassProbe(raw::Device *dev)
{
    watchSplitCapture(dev);
    const bool renderServer = dev->role() == raw::Device::Role::RenderServer;
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 raw::armGlassProbe();
                                 if (renderServer)
                                     armRecord(true);
                                 armGlassMem(dev, true, raw::kGlassMemArmMs);
                                 os_log(rawLog(), "raw: glass probe armed g%u (debug notification)",
                                        raw::gGlassProbeGeneration.load(std::memory_order_relaxed));
                             });
    if (renderServer)
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-record", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     raw::gGlassProbeGeneration.fetch_add(1, std::memory_order_relaxed);
                                     armRecord(true);
                                     armGlassMem(dev, true, raw::kGlassMemArmMs);
                                 });
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe-off", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 raw::disarmGlassProbe();
                                 if (renderServer)
                                     armRecord(false);
                                 disarmGlassMem(dev, true);
                             });
    if (renderServer) {
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-mem-watch", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     armGlassMem(dev, false, raw::kGlassMemWatchMs);
                                 });
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-mem-watch-off", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     disarmGlassMem(dev, false);
                                 });
    }
    watchForced(gWholePrograms, "whole-programs");
    watchForced(gZeroNewBuffers, "zero-new-buffers");
    watchForced(gPlainMemory, "plain-memory", &dev->probePlainMemory);
    watchForced(gHalfViewRefuse, "half-view-refuse");
}

void glassProbeCommitted(RMCommandBuffer *cb)
{
    publishGlassMem(*gRM->raw);
    Record &r = record();
    raw::GlassCbEntry e;
    e.ticks = mach_absolute_time();
    if (!r.armed.load(std::memory_order_relaxed) || e.ticks >= r.until.load(std::memory_order_relaxed))
        return;
    for (size_t at = 0; at < cb->_plan.size(); at++) {
        const Cmd c = cb->_plan.legacy(at);
        e.draws += isPlanDraw(c.op);
        e.encoders += c.op == OpRender || c.op == OpCompute || c.op == OpBlit;
        if (c.op != OpRender || c.x >= cb->_passes.size())
            continue;
        const RMPass &pass = cb->_passes[c.x];
        bool level = false;
        for (const RMPass::Color &a : pass.color)
            level |= a.texture && a.level;
        e.levels += level;
    }
    std::lock_guard<raw::Mutex> l(r.lock);
    if (!r.armed || r.countToken < 0)
        return;
    notify_set_state(r.slotTokens[r.count % kRecordSlots], e.packed());
    r.count++;
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
    os_log(rawLog(), "raw: glass cb g%u #%u draws %u encoders %u levels %u (mach %llu)", r.generation, r.count, e.draws,
           e.encoders, e.levels, (unsigned long long)e.ticks);
}
#endif
