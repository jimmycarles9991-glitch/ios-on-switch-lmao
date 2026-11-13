// SPDX-License-Identifier: MIT
// Debug builds: where the submit worker's encode of a raw range spends its time (the rates line's enc=), by part, for
// the plugin's "encode" line. A part's time includes the parts nested in it, and its self time leaves them out:
// Resources, Loop and Tail are the encode's three phases; Loop holds State (which holds Apply), Pass (which holds
// SplitPlan), Fusion, Blit and Compute; Commit, in Tail, holds Prologues and Exec (each Submit of the command buffer's
// batches; execUses: the job uses they carried). Draw (which holds Prepare, and Prepare Geometry, Descriptors (which
// holds Textures), Barriers and SplitLaunch), PassEnd, Kernel (every kernel call), GeometryAcquire and GeometryRelease
// are counted in whichever phase they run. Use (each resource a command buffer names), Uniforms (each setBytes chunk
// bound), Bind (a draw's state, root and depth/stencil words), Variant (a draw's program form) and PassPrologue (a
// pass's targets and clears, inside PassEnd) and PassBegin (the raw encoder's pass set up, inside Pass) are counted
// wherever they run too. Retire is the facade's work after the encode returned, outside its wall time. A span outside an encode (another thread, or the submit worker between
// encodes) costs one thread-local read.
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#include <atomic>
#include <mutex>
#include <type_traits>

namespace raw {

enum class EncodePart : uint8_t {
    Resources,
    Loop,
    State,
    Pass,
    SplitPlan,
    Fusion,
    PassEnd,
    Draw,
    Prepare,
    Geometry,
    Descriptors,
    Barriers,
    SplitLaunch,
    Blit,
    Compute,
    Tail,
    Commit,
    Prologues,
    Kernel,
    Exec,
    GeometryAcquire,
    GeometryRelease,
    Use,
    Uniforms,
    Bind,
    Variant,
    PassPrologue,
    Apply,
    Textures,
    Retire,
    PassBegin,
    Count
};

// Events of an encode, each counted on the encoding thread: Mallocs and Frees every allocation the thread made or freed
// (raw_encode_mallocs.h), Locks the mutexes taken on the encode path, Sends the Objective-C messages it sent, Applied
// the bindings the planned state applied, Uses the job uses named and UseAdds those that reached the job's list,
// DescriptorBuilds the binding tables built and DescriptorUploads those uploaded, GeometryVertices the vertices the CPU
// read for draw bounds
enum class EncodeCount : uint8_t {
    Mallocs,
    Frees,
    Locks,
    Sends,
    Applied,
    Uses,
    UseAdds,
    DescriptorBuilds,
    DescriptorUploads,
    GeometryVertices,
    Count
};

constexpr size_t kEncodeParts = size_t(EncodePart::Count), kEncodeCounts = size_t(EncodeCount::Count);

// The generic timer's virtual count without mach_absolute_time's barrier: a span's two reads may drift by a few
// instructions, which its sum over thousands of spans averages out. A Mac's need not run at mach_absolute_time's rate
// (an M4's counts 1 GHz under a 24 MHz timebase).
inline uint64_t encodeTicks()
{
#if defined(__arm64__)
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
    return mach_absolute_time();
#endif
}

// On a device the count is mach_absolute_time's own (its timebase converts it); a Mac's runs at CNTFRQ_EL0
inline double encodeTicksPerUs()
{
#if defined(__arm64__) && !TARGET_OS_IPHONE
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return double(hz) / 1e6;
#else
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    return 1000.0 * double(base.denom) / double(base.numer);
#endif
}

class EncodeSpan;

struct EncodeProfile {
    uint64_t ticks[kEncodeParts] = {};
    uint64_t self[kEncodeParts] = {};
    uint64_t calls[kEncodeParts] = {};
    uint64_t counts[kEncodeCounts] = {};
    uint64_t longestPrepare = 0, execUses = 0, execPushes = 0;
    bool mallocsCounted = false;
    EncodeSpan *open = nullptr;
};

inline thread_local EncodeProfile *tEncodeProfile = nullptr;

inline void noteExecUses(uint64_t uses)
{
    if (tEncodeProfile)
        tEncodeProfile->execUses += uses;
}

inline void noteExecPushes(uint64_t pushes)
{
    if (tEncodeProfile)
        tEncodeProfile->execPushes += pushes;
}

inline void countEncode(EncodeCount what, uint64_t n = 1)
{
    if (EncodeProfile *p = tEncodeProfile)
        p->counts[size_t(what)] += n;
}

class EncodeSpan {
public:
    explicit EncodeSpan(EncodePart part) : profile_(tEncodeProfile), part_(part)
    {
        if (!profile_)
            return;
        parent_ = profile_->open;
        profile_->open = this;
        began_ = encodeTicks();
    }

    ~EncodeSpan()
    {
        stop();
    }

    EncodeSpan(const EncodeSpan &) = delete;
    EncodeSpan &operator=(const EncodeSpan &) = delete;

    void stop()
    {
        if (!profile_)
            return;
        const uint64_t ticks = encodeTicks() - began_;
        profile_->ticks[size_t(part_)] += ticks;
        profile_->self[size_t(part_)] += ticks > nested_ ? ticks - nested_ : 0;
        profile_->calls[size_t(part_)]++;
        if (part_ == EncodePart::Prepare && ticks > profile_->longestPrepare)
            profile_->longestPrepare = ticks;
        if (profile_->open == this)
            profile_->open = parent_;
        if (parent_)
            parent_->nested_ += ticks;
        profile_ = nullptr;
    }

private:
    EncodeProfile *profile_;
    EncodeSpan *parent_ = nullptr;
    EncodePart part_;
    uint64_t began_ = 0, nested_ = 0;
};

// A lock on the encode path, counted
template <typename Mutex> class EncodeLock : public std::lock_guard<Mutex> {
public:
    explicit EncodeLock(Mutex &m) : std::lock_guard<Mutex>(m)
    {
        countEncode(EncodeCount::Locks);
    }
};

// What one span adds to the part it is nested in, in ticks per 1024 spans, measured once: a part's time holds its
// nested spans' count times this, which the encode line prints so a reader can take it out (Use and Uniforms are
// hundreds of short spans a command buffer)
inline uint64_t spanCostTicksPer1024()
{
    static const uint64_t cost = [] {
        EncodeProfile scratch;
        EncodeProfile *const saved = tEncodeProfile;
        tEncodeProfile = &scratch;
        const uint64_t start = encodeTicks();
        for (int i = 0; i < 1024; ++i) {
            EncodeSpan span(EncodePart::Use);
        }
        const uint64_t ticks = encodeTicks() - start;
        tEncodeProfile = saved;
        return ticks;
    }();
    return cost;
}

// The encodes' profiles since the device opened, each folded in once its encode returned (the submit worker's only)
struct EncodeTotals {
    std::atomic<uint64_t> encodes{0}, wallTicks{0}, longestPrepare{0}, execUses{0}, execPushes{0}, mallocEncodes{0};
    std::atomic<uint64_t> ticks[kEncodeParts] = {}, self[kEncodeParts] = {}, calls[kEncodeParts] = {};
    std::atomic<uint64_t> counts[kEncodeCounts] = {};

    void add(const EncodeProfile &p, uint64_t wall)
    {
        encodes.fetch_add(1, std::memory_order_relaxed);
        wallTicks.fetch_add(wall, std::memory_order_relaxed);
        execUses.fetch_add(p.execUses, std::memory_order_relaxed);
        execPushes.fetch_add(p.execPushes, std::memory_order_relaxed);
        mallocEncodes.fetch_add(p.mallocsCounted, std::memory_order_relaxed);
        for (size_t i = 0; i < kEncodeParts; ++i) {
            if (!p.calls[i])
                continue;
            ticks[i].fetch_add(p.ticks[i], std::memory_order_relaxed);
            self[i].fetch_add(p.self[i], std::memory_order_relaxed);
            calls[i].fetch_add(p.calls[i], std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kEncodeCounts; ++i)
            if (p.counts[i])
                counts[i].fetch_add(p.counts[i], std::memory_order_relaxed);
        uint64_t seen = longestPrepare.load(std::memory_order_relaxed);
        while (p.longestPrepare > seen &&
               !longestPrepare.compare_exchange_weak(seen, p.longestPrepare, std::memory_order_relaxed)) {
        }
    }
};

}  // namespace raw

#define RAW_ENCODE_SPAN_JOIN2(a, b) a##b
#define RAW_ENCODE_SPAN_JOIN(a, b)  RAW_ENCODE_SPAN_JOIN2(a, b)
#define RAW_ENCODE_SPAN(part) const raw::EncodeSpan RAW_ENCODE_SPAN_JOIN(rawEncodeSpan, __LINE__)(raw::EncodePart::part)
#define RAW_ENCODE_BEGIN(name, part) raw::EncodeSpan name(raw::EncodePart::part)
#define RAW_ENCODE_END(name)         name.stop()
#define RAW_ENCODE_COUNT(what, ...)  raw::countEncode(raw::EncodeCount::what, ##__VA_ARGS__)
#define RAW_ENCODE_LOCK(name, mutex) const raw::EncodeLock<std::remove_reference_t<decltype(mutex)>> name(mutex)
#else
#include <mutex>
#include <type_traits>
#define RAW_ENCODE_SPAN(part)        static_cast<void>(0)
#define RAW_ENCODE_BEGIN(name, part) static_cast<void>(0)
#define RAW_ENCODE_END(name)         static_cast<void>(0)
#define RAW_ENCODE_COUNT(what, ...)  static_cast<void>(0)
#define RAW_ENCODE_LOCK(name, mutex) const std::lock_guard<std::remove_reference_t<decltype(mutex)>> name(mutex)
#endif
