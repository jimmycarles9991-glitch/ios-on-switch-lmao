// SPDX-License-Identifier: MIT
// The user side of the frame timeline (debug builds only): a kdebug event at each step of a command buffer, for ktrace, and one
// "SwitchFrameUser:" line per slow command buffer (slow() says which), for the pulled log archive. Times are mach ticks, the
// kernel's "SwitchFrame:" clock; tools/frames joins the two by time.
#pragma once

#if (!defined(SWITCH_DEBUG) || SWITCH_DEBUG) && defined(SWITCH_STATS_FRAME) && SWITCH_STATS_FRAME
#define SWITCH_FRAME_TIMELINE 1
#else
#define SWITCH_FRAME_TIMELINE 0
#endif

#if SWITCH_FRAME_TIMELINE
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <utility>

extern "C" int kdebug_trace(uint32_t debugid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

namespace switch_frame {

// KDBG_CODE(DBG_APPS, 'S', code): r24-gpu-kernel's assignment, arg1 the command buffer's sequence number
enum Code : uint32_t {
    kCommit = 1,
    kRawEncode = 2,
    kReplay = 3,
    kReplayCommitted = 4,
    kOrderWait = 5,
    kFence = 6,
    kHandlersRun = 7,
    kM2vCommit = 8,
    kScheduled = 9,
    kCompleted = 10,
};

enum Phase : uint32_t {
    kPoint = 0,
    kStart = 1,
    kEnd = 2
};

inline void trace(Code code, Phase phase, uint64_t seq, uint64_t arg2 = 0)
{
    kdebug_trace(33u << 24 | uint32_t('S') << 16 | uint32_t(code) << 2 | phase, seq, arg2, 0, 0);
}

enum Field : unsigned {
    Previous,  // this thread's commit before this one
    Commit,
    Back,  // the commit call returned
    EncodeStart,
    EncodeEnd,
    ReplayStart,
    ReplayEnd,
    Returned,  // the replay's m2v commit returned
    OrderWaitStart,
    OrderWaitEnd,
    SlotStart,
    SlotEnd,
    Fence,
    Done,
    Made,       // the command buffer was made
    WaitStart,  // a thread first blocked in its waitUntilScheduled
    Scheduled,  // its scheduled handlers ran: what waitUntilScheduled waits for
    FieldCount
};

// Return addresses of the commit call's callers, nearest first: which of the client's paths committed (Core Animation's
// MetalContext::flush, and what called it: a frame's end or a new render encoder past its buffer pool's limit).
constexpr unsigned kSiteFrames = 5;

struct Site {
    uintptr_t pc[kSiteFrames] = {};
};

// The frame records (fp, lr) above `frame`, a frame of this thread, within its stack.
inline Site callers(const void *frame)
{
    Site site;
    pthread_t self = pthread_self();
    const uintptr_t top = uintptr_t(pthread_get_stackaddr_np(self)), bottom = top - pthread_get_stacksize_np(self);
    uintptr_t fp = uintptr_t(frame);
    for (unsigned i = 0; i < kSiteFrames && fp >= bottom && fp + 16 <= top && !(fp & 7); i++) {
        const uintptr_t *record = reinterpret_cast<const uintptr_t *>(fp);
        site.pc[i] = record[1];
        if (record[0] <= fp)
            break;
        fp = record[0];
    }
    return site;
}

// "QuartzCore+0x85660,...": each address as its image and offset (comma-joined, so tools/frames reads no fields in it)
inline void describe(const Site &site, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < kSiteFrames && site.pc[i] && at + 1 < size; i++) {
        Dl_info info = {};
        const char *image = "?";
        uintptr_t offset = site.pc[i];
        if (dladdr(reinterpret_cast<const void *>(site.pc[i]), &info) && info.dli_fbase) {
            const char *slash = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
            image = slash ? slash + 1 : info.dli_fname ? info.dli_fname : "?";
            offset = site.pc[i] - uintptr_t(info.dli_fbase);
        }
        const int n = snprintf(out + at, size - at, "%s%s+0x%lx", i ? "," : "", image, (unsigned long)offset);
        at = n < 0 ? at : at + size_t(n) < size ? at + size_t(n) : size - 1;
    }
    if (!at)
        snprintf(out, size, "-");
}

// Surface work in the process: Metal textures made over IOSurfaces, kernel imports made and reused
struct Churn {
    uint64_t surfaces = 0, imports = 0, hits = 0;
};

// Time inside the driver's own Metal entry points on this thread, outermost call only, by kind: set against the time
// between a command buffer's making and its commit, it splits the client's recording cost between it and the driver.
enum Api : unsigned {
    ApiDraw,
    ApiBind,
    ApiState,
    ApiEncoder,
    ApiObject,
    ApiKinds
};

struct ApiTotals {
    uint64_t ticks[ApiKinds] = {};
    uint64_t calls[ApiKinds] = {};
};

inline ApiTotals &apiTotals()
{
    static thread_local ApiTotals totals;
    return totals;
}

class ApiScope {
public:
    explicit ApiScope(Api kind) : kind(kind)
    {
        if (depth()++ == 0)
            start = mach_absolute_time();
    }

    ~ApiScope()
    {
        if (--depth() == 0) {
            ApiTotals &t = apiTotals();
            t.ticks[kind] += mach_absolute_time() - start;
            t.calls[kind]++;
        }
    }

    ApiScope(const ApiScope &) = delete;
    ApiScope &operator=(const ApiScope &) = delete;

private:
    static unsigned &depth()
    {
        static thread_local unsigned d = 0;
        return d;
    }

    Api kind;
    uint64_t start = 0;
};

#define SWITCH_API(kind) const switch_frame::ApiScope switchApiScope(switch_frame::kind)

// This thread's time blocked in waitUntilScheduled since its last commit.
inline uint64_t &blockedTicks()
{
    static thread_local uint64_t ticks = 0;
    return ticks;
}

// A commit site's own per-thread state: its previous commit, where it was called from, the process's churn totals then, and
// whether that commit belonged to a slow frame
struct Since {
    uint64_t commit = 0;
    Site site;
    Churn totals;
    bool slowFrame = false;
};

struct Stamps {
    uint64_t seq = 0;
    Churn churn;  // since this thread's previous commit at the same site
    Site site, previousSite;
    ApiTotals apiAtMade,
        api;  // the making thread's totals then; from then to the commit, when made and committed there
    pthread_t madeOn = nullptr;
    uint64_t blockedBefore = 0;  // the committing thread's time blocked in waitUntilScheduled since its previous commit
    bool slowFrame = false;      // committed in a slow frame (frameSlow)
    std::atomic<uint64_t> at[FieldCount] = {};

    void set(Field f, uint64_t ticks)
    {
        at[f].store(ticks, std::memory_order_relaxed);
    }

    uint64_t mark(Field f)
    {
        const uint64_t t = mach_absolute_time();
        set(f, t);
        return t;
    }

    uint64_t get(Field f) const
    {
        return at[f].load(std::memory_order_relaxed);
    }
};

// When the command buffer is made: when, and the making thread's driver time so far.
inline void made(Stamps &s)
{
    s.mark(Made);
    s.apiAtMade = apiTotals();
    s.madeOn = pthread_self();
}

inline uint64_t ticksPerMs()
{
    static const uint64_t perMs = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return tb.numer ? 1000000ull * tb.denom / tb.numer : 24000ull;
    }();
    return perMs;
}

// A waitUntilScheduled that blocks: the command buffer's first wait start, and the thread's time blocked.
class WaitScope {
public:
    explicit WaitScope(Stamps &s) : start(mach_absolute_time())
    {
        if (!s.get(WaitStart))
            s.set(WaitStart, start);
    }

    ~WaitScope()
    {
        blockedTicks() += mach_absolute_time() - start;
    }

    WaitScope(const WaitScope &) = delete;
    WaitScope &operator=(const WaitScope &) = delete;

private:
    uint64_t start;
};

// A commit opens a slow frame when it comes kSlowGapMs or more after the thread's previous one and the thread was not idle
// for kIdleMs of that: a gap it spent blocked in waitUntilScheduled or recording this command buffer is a stall, a longer
// one spent elsewhere a screen that went still. The thread's next commits under kSameFrameMs apart are the same frame's.
constexpr uint64_t kSlowGapMs = 20, kIdleMs = 250, kSameFrameMs = 8, kSlowCallMs = 8, kSlowWaitMs = 8;

inline bool frameSlow(const Stamps &s, bool previousSlow)
{
    const uint64_t ms = ticksPerMs();
    const uint64_t commit = s.get(Commit), previous = s.get(Previous), made = s.get(Made);
    if (!previous || commit <= previous)
        return false;
    const uint64_t gap = commit - previous;
    if (previousSlow && gap < kSameFrameMs * ms)
        return true;
    const uint64_t recording = made > previous && commit > made ? commit - made : 0;
    const uint64_t busy = s.blockedBefore + recording < gap ? s.blockedBefore + recording : gap;
    return gap >= kSlowGapMs * ms && gap - busy < kIdleMs * ms;
}

// At the commit call's entry: the command buffer's sequence number and the stamps that start it. `since` is the caller's own
// per-thread state: the facade's and the m2v frontend's commits interleave on one thread when a replay commits.
// `frame` is the commit method's own frame (__builtin_frame_address(0)), for its callers; null leaves them out.
inline void committed(Stamps &s, Since &since, const Churn &totals, const void *frame = nullptr)
{
    static std::atomic<uint64_t> sequence{0};
    s.seq = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t t = s.mark(Commit);
    s.set(Previous, since.commit);
    since.commit = t;
    s.blockedBefore = std::exchange(blockedTicks(), 0);
    s.slowFrame = since.slowFrame = frameSlow(s, since.slowFrame);
    if (frame) {
        s.site = callers(frame);
        s.previousSite = since.site;
        since.site = s.site;
    }
    if (s.madeOn == pthread_self()) {
        const ApiTotals &now = apiTotals();
        for (unsigned k = 0; k < ApiKinds; k++) {
            s.api.ticks[k] = now.ticks[k] - s.apiAtMade.ticks[k];
            s.api.calls[k] = now.calls[k] - s.apiAtMade.calls[k];
        }
    }
    s.churn = {totals.surfaces - since.totals.surfaces, totals.imports - since.totals.imports,
               totals.hits - since.totals.hits};
    since.totals = totals;
    trace(kCommit, kPoint, s.seq);
}

// Why a command buffer is slow, a bit each: committed in a slow frame (frameSlow: every command buffer of it), a commit call
// over kSlowCallMs, a waitUntilScheduled blocked over kSlowWaitMs until its scheduled handlers ran, a replay on m2v
enum Slow : unsigned {
    kSlowFrame = 1,
    kSlowCall = 2,
    kSlowWait = 4,
    kSlowReplay = 8
};

inline unsigned slowReasons(const Stamps &s)
{
    const uint64_t ms = ticksPerMs();
    const uint64_t commit = s.get(Commit), back = s.get(Back);
    const uint64_t waited = s.get(WaitStart), scheduled = s.get(Scheduled);
    unsigned why = s.slowFrame ? kSlowFrame : 0;
    if (back > commit && back - commit > kSlowCallMs * ms)
        why |= kSlowCall;
    if (waited && scheduled > waited && scheduled - waited > kSlowWaitMs * ms)
        why |= kSlowWait;
    if (s.get(ReplayStart))
        why |= kSlowReplay;
    return why;
}

inline bool slow(const Stamps &s)
{
    return slowReasons(s) != 0;
}

// slowReasons' bits by name, comma-joined, for the line's "why"
inline void describeSlow(unsigned why, char *out, size_t size)
{
    static const char *const names[] = {"frame", "call", "wait", "replay"};
    size_t at = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && at + 1 < size; i++) {
        if (!(why & (1u << i)))
            continue;
        const int w = snprintf(out + at, size - at, "%s%s", at ? "," : "", names[i]);
        at = w < 0 ? at : at + size_t(w) < size ? at + size_t(w) : size - 1;
    }
}

// After the completed handlers ran: the kdebug event, and the line when the command buffer was slow (at most
// kLinesPerSecond, the ones over that counted into the next line's "dropped"). The line is one string: each field is one
// add() below, "name start end" in ticks for a span tools/frames places on the frame, durations comma-joined in api.
inline void finished(Stamps &s)
{
    static constexpr uint32_t kLinesPerSecond = 60;  // every command buffer of a 20 Hz compositor's slow frames
    s.mark(Done);
    trace(kHandlersRun, kPoint, s.seq);
    const unsigned why = slowReasons(s);
    if (!why)
        return;
    static std::atomic<uint64_t> windowStart{0};
    static std::atomic<uint32_t> inWindow{0}, dropped{0};
    const uint64_t now = s.get(Done);
    uint64_t start = windowStart.load(std::memory_order_relaxed);
    if (now - start > 1000 * ticksPerMs() && windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed))
        inWindow.store(0, std::memory_order_relaxed);
    if (inWindow.fetch_add(1, std::memory_order_relaxed) >= kLinesPerSecond) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "frame");
    const auto g = [&s](Field f) {
        return (unsigned long long)s.get(f);
    };
    const uint64_t perUs = ticksPerMs() / 1000 ? ticksPerMs() / 1000 : 1;
    char line[1024];  // about 590 bytes used with both sites: the rest is for the fields still to come
    size_t at = 0;
    auto add = [&](const char *format, auto... values) {
        if (at + 1 >= sizeof(line))
            return;
        const int w = snprintf(line + at, sizeof(line) - at, format, values...);
        at = w < 0 ? at : at + size_t(w) < sizeof(line) ? at + size_t(w) : sizeof(line) - 1;
    };
    add("cb %llu commit %llu enc %llu %llu replay %llu %llu ret %llu owait %llu %llu fence %llu done %llu back %llu",
        (unsigned long long)s.seq, g(Commit), g(EncodeStart), g(EncodeEnd), g(ReplayStart), g(ReplayEnd), g(Returned),
        g(OrderWaitStart), g(OrderWaitEnd), g(Fence), g(Done), g(Back));
    add(" slot %llu %llu prev %llu ws %llu %llu dropped %u", g(SlotStart), g(SlotEnd), g(Previous), g(WaitStart),
        g(Scheduled), dropped.exchange(0, std::memory_order_relaxed));
    add(" surf %llu imp %llu hit %llu made %llu", (unsigned long long)s.churn.surfaces,
        (unsigned long long)s.churn.imports, (unsigned long long)s.churn.hits, g(Made));
    char reasons[32];
    describeSlow(why, reasons, sizeof(reasons));
    add(" why %s", reasons);
    // Comma-joined, so tools/frames reads no time spans in it.
    const ApiTotals &a = s.api;
    add(" api "
        "draw=%lluus/%llu,bind=%lluus/%llu,state=%lluus/%llu,encoder=%lluus/%llu,object=%lluus/%llu,blocked=%lluus",
        a.ticks[ApiDraw] / perUs, a.calls[ApiDraw], a.ticks[ApiBind] / perUs, a.calls[ApiBind],
        a.ticks[ApiState] / perUs, a.calls[ApiState], a.ticks[ApiEncoder] / perUs, a.calls[ApiEncoder],
        a.ticks[ApiObject] / perUs, a.calls[ApiObject], (unsigned long long)(s.blockedBefore / perUs));
    char sites[2 * kSiteFrames * 48];
    describe(s.site, sites, sizeof(sites) / 2);
    const size_t own = strlen(sites);
    snprintf(sites + own, sizeof(sites) - own, " prevsite ");
    describe(s.previousSite, sites + strlen(sites), sizeof(sites) - strlen(sites));
    add(" site %s", sites);
    os_log(log, "SwitchFrameUser: %{public}s %{public}s", getprogname(), line);
}

}  // namespace switch_frame
#else
#define SWITCH_API(kind) ((void)0)
#endif
