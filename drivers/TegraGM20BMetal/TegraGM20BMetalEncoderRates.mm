// SPDX-License-Identifier: MIT
// Debug builds' "frame" rates line: what a frame's CPU work met in the window (frame_cpu_counts.h)
#import "TegraGM20BMetalPipeline.h"
#include "object_pool.h"
#include "scheduled_wait.h"
#include <cstdarg>

#if SWITCH_METAL_DIAGNOSTICS
namespace {

struct LateWriteSample {
    struct BufferEntry {
        __unsafe_unretained SwitchMetalBuffer *buffer;
        size_t length;
        uint64_t hash;
    };

    struct TextureEntry {
        __unsafe_unretained SwitchMetalTexture *root;
        uint32_t replaceCount;
        uint32_t surfaceSeed;
        bool hasSurface;
    };

    static constexpr size_t kMaxBuffers = 4, kMaxTextures = 8, kMaxHashBytes = 4096;
    BufferEntry buffers[kMaxBuffers];
    TextureEntry textures[kMaxTextures];
    size_t bufferCount = 0, textureCount = 0;
};

LateWriteSample gLateWriteSlot;
std::atomic<void *> gLateWriteOwner{nullptr};

uint64_t hashPrefix(const uint8_t *bytes, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

// A wait this long is named on its own line as it ends, with where its time went
constexpr uint64_t kLongWaitUs = 50000;

const char *const kVetoNames[32] = {
    "-",      "counters", "shadows", "variant", "probes", "parallel", "ops",     "refused",
    "planar", "native",   "failed",  "other",   "device", "zcull",    "channel",
};

const char *const kLostNames[] = {"-", "short", "busy", "late"};
const char *const kWaitNames[gm20b::kHeadWaits] = {"event", "promised", "compile", "signal", "order"};

uint64_t take(std::atomic<uint64_t> &v)
{
    return v.exchange(0, std::memory_order_relaxed);
}

struct Line {
    char *out;
    size_t size, used = 0;
};

__attribute__((format(printf, 2, 3))) void put(Line &line, const char *format, ...)
{
    if (line.used + 1 >= line.size)
        return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line.out + line.used, line.size - line.used, format, args);
    va_end(args);
    line.used = n < 0 ? line.used : std::min(line.size - 1, line.used + size_t(n));
}

}  // namespace

void gm20bNoteTaken(SwitchMetalCommandBuffer *cb)
{
    gm20b::ScheduleNotes &n = cb->_scheduleNotes;
    const uint64_t now = nowUs();
    uint64_t none = 0;
    n.takenUs.compare_exchange_strong(none, now, std::memory_order_relaxed);
    if (const uint64_t at = n.parkedAtUs.exchange(0, std::memory_order_relaxed)) {
        const uint64_t parked = now - at;
        n.parkedUs.fetch_add(parked, std::memory_order_relaxed);
        const unsigned why = n.parkedWhy.load(std::memory_order_relaxed);
        gm20b::frameCpuCounts().parks[why].fetch_add(1, std::memory_order_relaxed);
        gm20b::frameCpuCounts().parkedUs[why].fetch_add(parked, std::memory_order_relaxed);
    }
}

void gm20bNoteParked(SwitchMetalCommandBuffer *cb, unsigned why)
{
    gm20b::ScheduleNotes &n = cb->_scheduleNotes;
    uint64_t none = 0;
    if (n.parkedAtUs.compare_exchange_strong(none, nowUs(), std::memory_order_relaxed))
        n.parkedWhy.store(uint8_t(why < gm20b::kHeadWaits ? why : 0), std::memory_order_relaxed);
}

void gm20bNoteFinished(SwitchMetalCommandBuffer *cb)
{
    const uint32_t encoders = cb->_pipelineEncoders;
    if (!encoders)
        return;
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    c.buffers.fetch_add(1, std::memory_order_relaxed);
    // the last two encoders are encoded at the commit by design (gm20bPublishEncoder)
    if (encoders - std::min(encoders, cb->_pipelinedEncoders) <= 2)
        c.ahead.fetch_add(1, std::memory_order_relaxed);
    for (uint32_t bits = cb->_pipelineVeto.load(std::memory_order_relaxed) | cb->_pipelineGivenBack |
                         cb->_aheadStopped.load(std::memory_order_relaxed);
         bits; bits &= bits - 1)
        c.vetoes[__builtin_ctz(bits)].fetch_add(1, std::memory_order_relaxed);
    const auto lost = cb->_handedOnce ? gm20b::AheadLost(cb->_scheduleNotes.aheadLost.load(std::memory_order_relaxed))
                                      : gm20b::AheadLost::Short;
    if (lost != gm20b::AheadLost::None)
        c.lost[size_t(lost)].fetch_add(1, std::memory_order_relaxed);
    if (cb->_kickedTicks > cb->_committedTicks && cb->_committedTicks) {
        static mach_timebase_info_data_t tb;
        if (!tb.denom)
            mach_timebase_info(&tb);
        const uint64_t us = (cb->_kickedTicks - cb->_committedTicks) * tb.numer / tb.denom / 1000;
        c.kicks.fetch_add(1, std::memory_order_relaxed);
        c.kickUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t longest = c.kickMaxUs.load(std::memory_order_relaxed);
        while (us > longest && !c.kickMaxUs.compare_exchange_weak(longest, us, std::memory_order_relaxed)) {
        }
    }
    const size_t enq = cb->_enqueueTiming < size_t(gm20b::EnqueueTiming::Count) ? cb->_enqueueTiming : 0;
    c.enqueues[enq].fetch_add(1, std::memory_order_relaxed);
}

void gm20bNoteEnqueue(SwitchMetalCommandBuffer *cb)
{
    if (!cb->_enqueueTiming && !cb->_committed.load(std::memory_order_relaxed))
        cb->_enqueueTiming = uint8_t(!cb->_active && cb->_plan.empty() ? gm20b::EnqueueTiming::BeforeFirstEncoder
                                                                       : gm20b::EnqueueTiming::BeforeCommit);
}

void gm20bSampleLateWrites(SwitchMetalCommandBuffer *cb, size_t from, size_t to)
{
    cb->_lateWriteChecked = true;
    const uint32_t period = gm20b::frameCpuCounts().lateWritePeriod.load(std::memory_order_relaxed);
    static std::atomic<uint64_t> seen{0};
    if (!period || (seen.fetch_add(1, std::memory_order_relaxed) + 1) % period != 0)
        return;
    void *expected = nullptr;
    if (!gLateWriteOwner.compare_exchange_strong(expected, (__bridge void *)cb, std::memory_order_acq_rel))
        return;
    LateWriteSample &sample = gLateWriteSlot;
    sample.bufferCount = 0;
    sample.textureCount = 0;
    void *written[32];
    size_t writtenCount = 0;
    cb->_plan.forEachOperationUse(from, to, [&](void *object, uint8_t access) {
        if (!(access & gm20b::ResourceUse::Write) || writtenCount >= 32)
            return;
        for (size_t i = 0; i < writtenCount; ++i)
            if (written[i] == object)
                return;
        written[writtenCount++] = object;
    });
    const auto wasWritten = [&](void *object) {
        for (size_t i = 0; i < writtenCount; ++i)
            if (written[i] == object)
                return true;
        return false;
    };
    cb->_plan.forEachOperationUse(from, to, [&](void *object, uint8_t access) {
        if (!object || !(access & (gm20b::ResourceUse::Read | gm20b::ResourceUse::Sample)))
            return;
        const SwitchMetalResourceFacts *facts = cb->_plan.factsOf(object);
        if (!facts)
            return;
        if (facts->kind == SwitchMetalResourceKind::Buffer && sample.bufferCount < LateWriteSample::kMaxBuffers) {
            __unsafe_unretained SwitchMetalBuffer *b = (__bridge SwitchMetalBuffer *)object;
            for (size_t i = 0; i < sample.bufferCount; ++i)
                if (sample.buffers[i].buffer == b)
                    return;
            if (b->_raw.hostVisible && b->_raw.cpu && b->_raw.length && !b->_raw.arena && !b->_aliasable &&
                !b->_gpuDirty.load(std::memory_order_acquire)) {
                const size_t n = std::min<size_t>(size_t(b->_raw.length), LateWriteSample::kMaxHashBytes);
                sample.buffers[sample.bufferCount++] = {b, n, hashPrefix(b->_raw.cpu, n)};
            }
        } else if (facts->kind == SwitchMetalResourceKind::Texture && !(access & gm20b::ResourceUse::Write) &&
                   sample.textureCount < LateWriteSample::kMaxTextures) {
            __unsafe_unretained SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)object;
            __unsafe_unretained SwitchMetalTexture *root = t->_levelsOwner ?: t;
            if (wasWritten((__bridge void *)t) || wasWritten((__bridge void *)root))
                return;
            for (size_t i = 0; i < sample.textureCount; ++i)
                if (sample.textures[i].root == root)
                    return;
            sample.textures[sample.textureCount++] = {root, root->_replaceCount.load(std::memory_order_relaxed),
                                                      root->_surface ? IOSurfaceGetSeed(root->_surface) : 0u,
                                                      root->_surface != nullptr};
        }
    });
    if (sample.bufferCount || sample.textureCount)
        cb->_lateWriteSampled = true;
    else
        gLateWriteOwner.store(nullptr, std::memory_order_release);
}

void gm20bCheckLateWrites(SwitchMetalCommandBuffer *cb)
{
    cb->_lateWriteSampled = false;
    if (gLateWriteOwner.load(std::memory_order_acquire) != (__bridge void *)cb)
        return;
    const LateWriteSample &sample = gLateWriteSlot;
    bool bufChanged = false, replaceChanged = false, surfaceChanged = false;
    for (size_t i = 0; i < sample.bufferCount; ++i) {
        const auto &e = sample.buffers[i];
        if (hashPrefix(e.buffer->_raw.cpu, e.length) != e.hash) {
            bufChanged = true;
            break;
        }
    }
    for (size_t i = 0; i < sample.textureCount; ++i) {
        const auto &e = sample.textures[i];
        if (e.root->_replaceCount.load(std::memory_order_relaxed) != e.replaceCount)
            replaceChanged = true;
        if (e.hasSurface && IOSurfaceGetSeed(e.root->_surface) != e.surfaceSeed)
            surfaceChanged = true;
    }
    gLateWriteOwner.store(nullptr, std::memory_order_release);
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    c.lateWriteSampled.fetch_add(1, std::memory_order_relaxed);
    if (bufChanged)
        c.lateWriteBuffer.fetch_add(1, std::memory_order_relaxed);
    if (replaceChanged)
        c.lateWriteReplace.fetch_add(1, std::memory_order_relaxed);
    if (surfaceChanged)
        c.lateWriteSurface.fetch_add(1, std::memory_order_relaxed);
}

void gm20bDropLateWrites(SwitchMetalCommandBuffer *cb)
{
    cb->_lateWriteSampled = false;
    void *expected = (__bridge void *)cb;
    gLateWriteOwner.compare_exchange_strong(expected, nullptr, std::memory_order_release);
}

void gm20bNoteScheduledWait(SwitchMetalCommandBuffer *cb, uint64_t fromUs, uint64_t toUs)
{
    const gm20b::ScheduledWait w = gm20b::splitScheduledWait(cb->_scheduleNotes, fromUs, toUs);
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.waitedUs.fetch_add(w.total(), std::memory_order_relaxed);
    c.waitQueued.fetch_add(w.queued, std::memory_order_relaxed);
    c.waitParked.fetch_add(w.parked, std::memory_order_relaxed);
    c.waitEncoding.fetch_add(w.encoding, std::memory_order_relaxed);
    c.waitHandlers.fetch_add(w.handlers, std::memory_order_relaxed);
    c.waitOther.fetch_add(w.other, std::memory_order_relaxed);
    uint64_t longest = c.longestWait.load(std::memory_order_relaxed);
    while (w.total() > longest && !c.longestWait.compare_exchange_weak(longest, w.total(), std::memory_order_relaxed)) {
    }
    if (w.total() > longest) {
        const uint64_t parts[5] = {w.queued, w.parked, w.encoding, w.handlers, w.other};
        for (size_t i = 0; i < 5; i++)
            c.longestParts[i].store(parts[i], std::memory_order_relaxed);
    }
    if (w.total() < kLongWaitUs)
        return;
    const unsigned why = cb->_scheduleNotes.parkedWhy.load(std::memory_order_relaxed);
    os_log(gm20bLog(),
           "SwitchMetal: waitUntilScheduled blocked %llu us on queue %{public}s: before the worker took it %llu, "
           "parked %llu (last for %{public}s), encoding %llu, its scheduled handlers %llu, other %llu",
           (unsigned long long)w.total(), cb->_queue.label.UTF8String ?: "-", (unsigned long long)w.queued,
           (unsigned long long)w.parked, w.parked ? kWaitNames[why < gm20b::kHeadWaits ? why : 0] : "-",
           (unsigned long long)w.encoding, (unsigned long long)w.handlers, (unsigned long long)w.other);
}
#endif

size_t metal_frame_rates(char *out, size_t size)
{
#if SWITCH_METAL_DIAGNOSTICS
    if (!gRM || !size)
        return 0;
    gm20b::FrameCpuCounts &c = gm20b::frameCpuCounts();
    uint64_t passes[size_t(gm20b::PassOpen::Count)];
    uint64_t opened = 0;
    for (size_t i = 0; i < size_t(gm20b::PassOpen::Count); i++)
        opened += passes[i] = take(c.passes[i]);
    const uint64_t buffers = take(c.buffers), waits = take(c.waits);
    if (!opened && !buffers && !waits)
        return 0;
    Line line{out, size};
    using P = gm20b::PassOpen;
    put(line, "frame pass=%llu hit=%llu kept=%llu unkept=%llu unkeyed=%llu counters=%llu owned=%llu",
        (unsigned long long)opened, (unsigned long long)passes[size_t(P::Hit)],
        (unsigned long long)passes[size_t(P::Kept)], (unsigned long long)passes[size_t(P::Unkept)],
        (unsigned long long)passes[size_t(P::Unkeyed)], (unsigned long long)passes[size_t(P::Counters)],
        (unsigned long long)take(c.passOwned));
    static uint64_t lastPipelined, lastSealed;
    const uint64_t pipelined = gRM->stats.pipelinedEncoders.load(std::memory_order_relaxed),
                   sealed = gRM->stats.sealedEncoders.load(std::memory_order_relaxed);
    const uint64_t kicks = take(c.kicks), kickUs = take(c.kickUs);
    put(line, " split=%llu kick=%llu/%lluus", (unsigned long long)take(c.splits),
        (unsigned long long)(kicks ? kickUs / kicks : 0), (unsigned long long)take(c.kickMaxUs));
    put(line, " ahead=%llu/%llu enc=%llu/%llu why=", (unsigned long long)take(c.ahead), (unsigned long long)buffers,
        (unsigned long long)(pipelined - lastPipelined), (unsigned long long)(sealed - lastSealed));
    lastPipelined = pipelined;
    lastSealed = sealed;
    bool any = false;
    for (unsigned bit = 0; bit < 32; bit++)
        if (const uint64_t n = take(c.vetoes[bit])) {
            put(line, "%s%s:%llu", any ? "," : "", kVetoNames[bit] ? kVetoNames[bit] : "?", (unsigned long long)n);
            any = true;
        }
    for (unsigned k = 1; k < 4; k++)
        if (const uint64_t n = take(c.lost[k])) {
            put(line, "%s%s:%llu", any ? "," : "", kLostNames[k], (unsigned long long)n);
            any = true;
        }
    if (!any)
        put(line, "-");
    const Class pooled[] = {[SwitchMetalRenderEncoder class], [SwitchMetalCommandBuffer class]};
    for (Class cls : pooled)
        put(line, " %s=%llu/%llu/%u/%u", cls == [SwitchMetalCommandBuffer class] ? "cbs" : "encoders",
            (unsigned long long)gm20bPoolMade(cls), (unsigned long long)gm20bPoolFreed(cls), gm20bPoolMost(cls),
            gm20bPoolKept(cls));
    put(line, " park=");
    any = false;
    for (unsigned k = 0; k < gm20b::kHeadWaits; k++)
        if (const uint64_t n = take(c.parks[k])) {
            put(line, "%s%s:%llu/%lluus", any ? "," : "", kWaitNames[k], (unsigned long long)n,
                (unsigned long long)take(c.parkedUs[k]));
            any = true;
        }
    if (!any)
        put(line, "-");
    put(line, " ws=%llu/%lluus(queued %llu parked %llu encoding %llu handlers %llu other %llu)",
        (unsigned long long)waits, (unsigned long long)take(c.waitedUs), (unsigned long long)take(c.waitQueued),
        (unsigned long long)take(c.waitParked), (unsigned long long)take(c.waitEncoding),
        (unsigned long long)take(c.waitHandlers), (unsigned long long)take(c.waitOther));
    put(line, " wsmax=%lluus(%llu %llu %llu %llu %llu)", (unsigned long long)take(c.longestWait),
        (unsigned long long)take(c.longestParts[0]), (unsigned long long)take(c.longestParts[1]),
        (unsigned long long)take(c.longestParts[2]), (unsigned long long)take(c.longestParts[3]),
        (unsigned long long)take(c.longestParts[4]));
    using E = gm20b::EnqueueTiming;
    put(line, " enq=%llu/%llu/%llu latew=%llu/%llu/%llu/%llu",
        (unsigned long long)take(c.enqueues[size_t(E::BeforeFirstEncoder)]),
        (unsigned long long)take(c.enqueues[size_t(E::BeforeCommit)]),
        (unsigned long long)take(c.enqueues[size_t(E::Never)]), (unsigned long long)take(c.lateWriteSampled),
        (unsigned long long)take(c.lateWriteBuffer), (unsigned long long)take(c.lateWriteReplace),
        (unsigned long long)take(c.lateWriteSurface));
    return line.used;
#else
    (void)out;
    (void)size;
    return 0;
#endif
}

#if SWITCH_METAL_DIAGNOSTICS
@implementation SwitchMetalDevice (FrameRates)

- (NSNumber *)switchMetalProbeLateWritePeriod:(NSNumber *)period
{
    return @(gm20b::frameCpuCounts().lateWritePeriod.exchange(period.unsignedIntValue, std::memory_order_relaxed));
}

@end
#endif
