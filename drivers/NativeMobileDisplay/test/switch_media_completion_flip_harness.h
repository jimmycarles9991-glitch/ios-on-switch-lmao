// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's windows B and C as SwitchMediaCompletionFlipDisplay.h, planeStage and the latch drive them,
// against a model of DC A: per window an assembly copy, the ARM copy its UPDATE takes and the active copy a boundary
// takes while its ACT_REQ is out (CSC and filter words are live). kEveryWindow models a controller whose any UPDATE
// arms and any ACT_REQ latches every window. The offer (planeFence), the completion (gpuComplete), its pass
// (finishPlaneFence, planeOut) and the frame end (planeFrameEnd) are NativeLegacyDisplay's as far as windows B and C
// see them; the rest is the real code.
#pragma once
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../TegraGM20B/SwitchGpuBacking.h"
#include "../../TegraHost1x/SwitchHost1xFence.h"
#include "../SwitchCompletionFlip.h"
#include "../SwitchDisplayComposition.h"
#include "../SwitchDisplayRetirement.h"
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchLandscapeWindows.h"

namespace media_flip_test {

inline std::vector<std::string> gLog;
inline int gFenceResult = 0;
inline unsigned gFences = 0;

// host1x's filter waiters as the display registers them, with what each producer's job record says in the filter.
struct FilterWaiter {
    void (*fn)(void *);
    void *ctx;
    uint32_t threshold;
};

inline std::vector<FilterWaiter> gWaiters;
inline int gWaitResult = 0, gProducerCertain = 1;
inline std::set<uint64_t> gUncertain, gDoneAtArm;  // producers' fences: still running in the filter, done at the arm

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    throw std::runtime_error(text);
}

// Interrupts or preemption off: completionFlipLock held, or host1x's syncpoint filter running. Nothing that blocks or
// logs may run then (XNU panics on a mutex with preemption disabled); IOLog throws here.
inline int gPreemptionOff = 0;

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    if (gPreemptionOff)
        throw std::runtime_error(std::string("IOLog with preemption disabled: ") + format);
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    gLog.emplace_back(text);
}

inline int switch_emc_display_memory_ready()
{
    return 1;
}

inline int switch_emc_display_memory_reserve(const switch_display_memory_head *)
{
    return 0;
}

struct IOSimpleLock {
    bool held = false;
};

using IOInterruptState = int;

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    if (lock->held)
        throw std::runtime_error("completionFlipLock taken while held");
    lock->held = true;
    ++gPreemptionOff;
    return 0;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    lock->held = false;
    --gPreemptionOff;
}

inline uint64_t gClock = 1000;

inline uint64_t mach_absolute_time()
{
    return ++gClock;
}

inline struct {
    bool panicking = false;
} gPanicRestore;

inline void native_legacy_gpu_complete(void *, int)
{}

inline int switch_host1x_token_fence(const switch_gpu_backing_token *, uint32_t, void (*)(void *, int), void *)
{
    ++gFences;
    return gFenceResult;
}

struct DcModel {
    enum Promote {
        kPerWindow,
        kEveryWindow
    };

    static constexpr uint32_t kStateAccess = 0x040, kStateControl = 0x041;
    std::array<uint32_t, 4096> assembly{}, armed{}, active{};
    uint32_t requested = 0;
    Promote promote = kPerWindow;
    bool completionDropsAddresses = false, completing = false;
    std::vector<uint32_t> written;

    static int window(uint32_t word)
    {
        if (word >= 0xb80 && word < 0xc00)
            return 0;
        if (word >= 0xd80 && word < 0xe00)
            return 1;
        if (word >= 0xf80 && word < 0x1000)
            return 2;
        return -1;
    }

    void copy(std::array<uint32_t, 4096> &to, const std::array<uint32_t, 4096> &from, unsigned w)
    {
        const uint32_t first = w == 0 ? 0xb80 : w == 1 ? 0xd80 : 0xf80;
        for (uint32_t word = first; word < first + 0x80; ++word)
            to[word] = from[word];
    }

    void write(uint32_t word, uint32_t value)
    {
        written.push_back(word);
        if (word == kStateControl) {
            for (unsigned w = 0; w != 3; ++w)
                if ((value & (1u << (9 + w))) || (promote == kEveryWindow && (value & 0xf00u)))
                    copy(armed, assembly, w);
            requested |= value & 0xfu;
            return;
        }
        if (window(word) >= 0) {
            if (!(completing && completionDropsAddresses))
                assembly[word] = value;
        } else {
            assembly[word] = active[word] = value;
        }
    }

    uint32_t read(uint32_t word) const
    {
        if (word == kStateControl)
            return requested;
        if (window(word) >= 0)
            return (active[kStateAccess] & 1u) ? active[word] : assembly[word];
        return active[word];
    }

    void boundary()
    {
        for (unsigned w = 0; w != 3; ++w)
            if ((requested & (2u << w)) || (promote == kEveryWindow && requested))
                copy(active, armed, w);
        requested = 0;
    }
};

struct Words {
    DcModel *dc = nullptr;
    uint32_t base = 0;

    struct Ref {
        DcModel *dc;
        uint32_t word;

        operator uint32_t() const
        {
            return dc->read(word);
        }

        Ref &operator=(uint32_t value)
        {
            dc->write(word, value);
            return *this;
        }

        Ref &operator=(const Ref &other)
        {
            return *this = static_cast<uint32_t>(other);
        }
    };

    Ref operator[](uint32_t word) const
    {
        return {dc, base + word};
    }

    Words operator+(uint32_t offset) const
    {
        return {dc, base + offset};
    }
};

using SwitchMMIOWords = Words;

struct Display {
    using PlaneSet = switch_display::Composition;
    using Window = switch_landscape::Window;
    using PlaneCommits = switch_display::Compositions<PlaneSet>;
    using Commits = PlaneCommits;
    static constexpr int kNoBuffer = -1;
    static constexpr uint32_t kWinABase = 0xb80, kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kWinbufBBase = 0xdc0, kWinbufCBase = 0xfc0, kWincBBase = 0xc00, kWincCBase = 0xe00;
    static constexpr uint32_t kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10, kWinCUpdate = 1 << 11;
    static constexpr uint32_t kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kGeneralActReq = 1;
    static constexpr uint32_t kWinBActHcntrSel = 1 << 4, kWinCActHcntrSel = 1 << 6;
    static constexpr uint32_t kWinBUnderflowInt = 1 << 9, kWinCUnderflowInt = 1 << 10;
    static constexpr uint32_t kDcRegActControl = 0x43, kDcStateControl = 0x41;
    static constexpr uint32_t kDcIntEnable = 0x39, kDcIntStatus = 0x37;
    static constexpr uint32_t kDcStateAccess = 0x40, kReadMuxActive = 1, kWriteMuxActive = 4;
    static constexpr unsigned kLatchFrames = 4;

    struct Slot {
        uint64_t low = 0;
    } planeSlots[8];

    struct {
        int held = kNoBuffer, pending = kNoBuffer;
    } ring;

    uint64_t entrySeq[8] = {};

    struct {
        bool active() const
        {
            return false;
        }
    } planeProbe;

    DcModel model;
    Words dc{&model};
    bool entryPlaneSet[4] = {};
    PlaneSet sets[Commits::kSlots + 1] = {};
    Commits planeLatch;
    PlaneSet planeTarget = {}, planeHeld = {}, planeFencing = {};
    bool planeHeldSet = false, planeBlending = false, planeFiltersWritten[2] = {};
    bool stats = false, gpuScanout = false, planesOn = true, planesAttached = true, copyPlaneJoined = false;
    bool hwSuspended = false, gpuDonePending = false, planeFencePending = false, transferred = false;
    int planeCsc[2][2] = {{-1, -1}, {-1, -1}}, gpuStatus = 0;
    uint32_t planeBlendSaved[3] = {}, pitchActControl = UINT32_MAX;
    uint64_t planeRequestedAt = 0, frameEnds = 10;
    unsigned stPlaneFlips = 0, stPlaneDropped = 0, deadlines = 0, releases = 0, planeUnderflowRun = 0;
    unsigned notShown = 0;
    IOSimpleLock lock, *completionFlipLock = nullptr;
    std::vector<uint64_t> shown;  // the take sequence of every set the latch saw taken
    std::function<void()> midFrameEnd;

    Display()
    {
        gPreemptionOff = 0;
        landscapeState = {};
        mediaFlip = {};
        mediaFlipsRefused = mediaFilterOff = false;
        mediaFlipUnverified = PlaneCommits::kNone;
        mediaFlipRequestedSeq = 0;
        planeLatch.values = sets;
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        if (!displayMemory.configure(mode, 720, 1280, false))
            throw std::runtime_error("display memory");
    }

    void scanoutMemoryBarrier()
    {}

    void planeReleaseIdle()
    {
        ++releases;
    }

    const void *firstUntaken() const
    {
        return nullptr;
    }

    bool planeWindowIdle() const
    {
        return false;
    }

    static bool windowProbeBusy()
    {
        return false;
    }

    void getDisplayDimensions(uint32_t *width, uint32_t *height)
    {
        *width = 720;
        *height = 1280;
    }

    uint32_t planeSlotIova(int slot) const
    {
        return 0x10000000 + uint32_t(slot) * 0x100000;
    }

    const PlaneSet &planeSetOf(int id) const
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    PlaneSet &planeSetOf(int id)
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    void armFlipDeadline()
    {
        ++deadlines;
    }

    void withdrawCompletionFlip()
    {}

    void startCompletionFlip()
    {
        completionFlipLock = &lock;
    }

    static bool completionFlipIdle()
    {
        return true;
    }

    void runCompletionPass()
    {
        if (!gpuDonePending)
            throw std::runtime_error("a written flip whose completion signalled nothing");
        gpuDonePending = false;
        finishPlaneFence(gpuStatus);
    }

    // planeFence and planeMediaFence for a set whose producers are VIC's or NVDEC's: the second, when there are two,
    // NVDEC's at fence seq + kSecond.
    static constexpr uint64_t kSecond = 1000000;

    bool offer(PlaneSet p, uint64_t seq, unsigned producers = 1)
    {
        if (transferred)
            throw std::runtime_error("an offer while a swap is in flight");
        p.seq = seq;
        swaps.push_back(seq);
        planeFencing = p;
        planeFencePending = gpuScanout = transferred = true;
        planeTarget = p;
        const switch_gpu_backing_token producer[2] = {{seq, 1, SWITCH_GPU_ENGINE_VIC, 161},
                                                      {seq + kSecond, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
        if (planeFenceWithMediaFlip(p, producer, producers) == 0)
            return true;
        planeFencePending = gpuScanout = transferred = false;
        planeNotShown();
        return false;
    }

    // host1x's syncpoint filter with every waiter's threshold reached (or only `threshold`'s); the stores land as a
    // completion's do.
    void filter(uint64_t threshold = 0)
    {
        model.completing = true;
        ++gPreemptionOff;
        for (const auto &waiter : gWaiters)
            if (!threshold || waiter.threshold == static_cast<uint32_t>(threshold))
                waiter.fn(waiter.ctx);
        --gPreemptionOff;
        model.completing = false;
    }

    // gpuComplete on the producer's thread, after the pass that retired its filter waiters.
    void complete(int status)
    {
        gWaiters.clear();
        status = mediaFlipVerdict(status);
        gpuStatus = status;
        gpuDonePending = true;
        model.completing = true;
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
        writeMediaFlipLocked(status);
        IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
        model.completing = false;
    }

    // gpuDoneFired.
    void donePass()
    {
        if (gpuDonePending)
            runCompletionPass();
    }

    void finishPlaneFence(int status)
    {
        mediaFlipPass();
        gpuScanout = planeFencePending = transferred = false;
        if (status || !planesOn) {
            planeNotShown();
        } else if (hwSuspended) {
            planeHeld = planeFencing;
            planeHeldSet = true;
        } else {
            planeOut(planeFencing);
        }
    }

    // The family's swaps as SwitchRetireDisplay.h retires them at a latch (latchRetire): each offered set's swap is
    // queued, and must complete exactly once.
    switch_display::Retirement scanoutRetirement;
    std::vector<uint64_t> swaps, retired, framesShown;

    uint64_t oldestPendingFrame() const
    {
        uint64_t first = 0;
        const auto include = [&first](uint64_t sequence) {
            if (sequence && (!first || sequence < first))
                first = sequence;
        };
        if (planeLatch.requested())
            include(planeSetOf(planeLatch.armed).seq);
        if (planeLatch.superseded >= 0)
            include(planeSetOf(planeLatch.superseded).seq);
        if (planeHeldSet)
            include(planeHeld.seq);
        if (planeFencePending)
            include(planeFencing.seq);
        return first;
    }

    void retireThrough(uint64_t seq, uint64_t shownAt)
    {
        scanoutRetirement.observe(seq, shownAt);
        const auto ready = scanoutRetirement.take(oldestPendingFrame());
        if (!ready.sequence)
            return;
        framesShown.push_back(ready.sequence);
        while (!swaps.empty() && swaps.front() <= ready.sequence) {
            if (std::count(retired.begin(), retired.end(), swaps.front()))
                throw std::runtime_error("a swap completed twice");
            retired.push_back(swaps.front());
            swaps.erase(swaps.begin());
        }
    }

    unsigned completions(uint64_t seq) const
    {
        return static_cast<unsigned>(std::count(retired.begin(), retired.end(), seq));
    }

    // NativePlanesDisplay.h retireUnshown: carried by the set still out, or retired now.
    void planeNotShown()
    {
        ++notShown;
        planeTarget = planeSetOf(planeLatch.requested() ? planeLatch.armed : planeLatch.active);
        if (planeLatch.requested())
            planeSetOf(planeLatch.armed).seq = planeFencing.seq;
        else if (planeHeldSet)
            planeHeld.seq = planeFencing.seq;
        else
            retireThrough(planeFencing.seq, mach_absolute_time());
    }

    void planeOut(const PlaneSet &p)
    {
        if (presentedByMediaFlip(p))
            return;
        planeFlipOnly(p);
    }

    // vblankTick's window B steps: takeCompletionFlip first, then planeFrameEnd's latch.
    void frameEnd()
    {
        takeMediaFlip();
        if (midFrameEnd)
            midFrameEnd();
        frameEnds = frameEnds + 1;
        if (hwSuspended || !planeLatchRequested())
            return;
        if (planeLatchedAtFrameEnd()) {
            shown.push_back(planeSetOf(planeLatch.active).seq);
            retireThrough(planeSetOf(planeLatch.active).seq, mach_absolute_time());
            if (planeHeldSet && planeLatch.mayWrite()) {
                planeHeldSet = false;
                planeFlipOnly(planeHeld);
            }
        } else if (frameEnds - planeRequestedAt >= kLatchFrames) {
            throw std::runtime_error("windows B and C did not latch in 4 frames (the overlay recovery)");
        }
    }

    // quiesceForSuspend (suspendCompletionFlip, planesQuiesce) and the resume.
    void suspend()
    {
        mediaFlipFromGate();
        const uint64_t shown = suspendShownSeq();
        planeLatch.settle();
        hwSuspended = true;
        retireSuspendShown(shown);
    }

    void resume()
    {
        hwSuspended = false;
        planesResume();
    }

#include "../SwitchLandscapeState.h"

    bool landscapeHolds(PlaneSet &)
    {
        return false;
    }

    void landscapeLeaveNow(const char *)
    {}

    void landscapeAbandonWant()
    {}

    bool landscapeHoldsKicks() const
    {
        return false;
    }

    uint32_t landscapeRequestBits() const
    {
        return switch_landscape::overlayRequests(landscapeOn());
    }

#define SWITCH_FRAME_TIMELINE 0
#include "../SwitchDisplayBandwidthDisplay.h"
#include "../SwitchOverlayRegisters.h"
#include "../SwitchPlaneAdmissionDrop.h"
#include "../SwitchCompositionRegisters.h"
#include "../SwitchOverlayRecovery.h"
#include "../SwitchPlanesResume.h"
#include "../SwitchMediaCompletionFlipDisplay.h"
#include "../SwitchSuspendRetire.h"
#undef SWITCH_FRAME_TIMELINE
};

inline Display::LandscapeState Display::landscapeState;

// A video frame in slot `slot` on window B, as SwitchPlanesDisplay builds one (NV12, BT.709).
inline Display::PlaneSet video(int slot, uint32_t size = (720u << 16) | 1280)
{
    Display::PlaneSet p = {};
    p.window[0].on = true;
    p.window[0].slot = slot;
    p.window[0].image.visible = p.window[0].image.yuv = true;
    p.window[0].image.matrix = native_planes::kBt709;
    p.window[0].image.options = native_planes::kWinEnable;
    p.window[0].image.size = size;
    p.window[0].image.prescaled = size;
    p.window[0].image.lineStride = 1280;
    p.window[0].image.start[1] = 1280u * 720u;
    return p;
}

}  // namespace media_flip_test

extern "C" inline int switch_host1x_filter_wait(uint32_t, uint32_t threshold, void (*fn)(void *), void *ctx,
                                                switch_host1x_filter_handle *handle)
{
    using namespace media_flip_test;
    *handle = threshold;
    if (!gWaitResult)
        gWaiters.push_back({fn, ctx, threshold});
    return gWaitResult;
}

extern "C" inline int switch_gpu_backing_token_filter_status(const switch_gpu_backing_token *token)
{
    return media_flip_test::gUncertain.count(token->value) ? 0 : media_flip_test::gProducerCertain;
}

extern "C" inline int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
    return media_flip_test::gDoneAtArm.count(token->value) ? 1 : 0;
}
