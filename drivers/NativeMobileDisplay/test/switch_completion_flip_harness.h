// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window A as SwitchCompletionFlipDisplay.h and SwitchScanoutStart.h drive it, against a model
// of DC A's window A: an assembly copy (written through WINDOW_HEADER or window A's own aperture), the ARM copy
// WIN_A_UPDATE takes, and the active copy a frame boundary takes while WIN_A_ACT_REQ is out. The gate's side is the real code; the
// offer, the completion's pass and the frame-end action are NativeLegacyDisplay's as far as window A sees them.
#pragma once
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../TegraDisplayFlip/TegraScanoutRing.h"
#include "../../TegraGM20B/SwitchGpuBacking.h"
#include "../../TegraHost1x/SwitchHost1xFence.h"
#include "../SwitchCompletionFlip.h"

namespace completion_flip_test {

// The producers' filters as the display registers with them: the GPU's present callout, host1x's last filter waiter
// (with what its VIC job's record says when the filter runs it).
inline void (*gGpuCallout)(void *, int) = nullptr;
inline void (*gWaiterFn)(void *) = nullptr;
inline void *gWaiterContext = nullptr;
inline switch_gpu_backing_token gWaiterToken{};
inline int gWaitResult = 0, gVicStatus = 1;
inline unsigned gWaits = 0;

struct OriginalLegacySwapRequest {
    uint64_t seq;
};

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    throw std::runtime_error(text);
}

// Interrupts or preemption off: an IOSimpleLock held, or a producer's primary filter running. XNU panics on any
// mutex, sleep or allocation then ("Attempt to take mutex ... with preemption disabled"), so every stub that stands for
// one of those throws.
inline int gPreemptionOff = 0;

inline void mayBlock(const char *what)
{
    if (gPreemptionOff)
        throw std::runtime_error(std::string(what) + " with preemption disabled");
}

struct IOLock {};

struct IORecursiveLock {};

inline void IOLockLock(IOLock *)
{
    mayBlock("IOLockLock");
}

inline void IORecursiveLockLock(IORecursiveLock *)
{
    mayBlock("IORecursiveLockLock");
}

inline void *IOMalloc(size_t size)
{
    mayBlock("IOMalloc");
    return ::operator new(size);
}

struct IOCommandGate {
    template <class... Args> int runAction(Args...)
    {
        mayBlock("IOCommandGate::runAction");
        return 0;
    }
};

// The boot framebuffer's mapping: its physical address takes the descriptor's IORecursiveLock.
struct IOMemoryMap {
    uint64_t physical = 0;

    uint64_t getPhysicalAddress() const
    {
        mayBlock("IOMemoryMap::getPhysicalAddress (IORecursiveLockLock)");
        return physical;
    }

    uint64_t getPhysicalSegment(uint64_t offset, uint64_t *, uint32_t = 0) const
    {
        mayBlock("IOMemoryMap::getPhysicalSegment (IORecursiveLockLock)");
        return physical + offset;
    }
};

inline std::vector<std::string> gLog;

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    mayBlock("IOLog");
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    gLog.emplace_back(text);
}

struct IOSimpleLock {
    bool held = false;
};

using IOInterruptState = int;

inline IOSimpleLock *IOSimpleLockAlloc()
{
    static IOSimpleLock lock;
    return &lock;
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    if (lock->held)
        throw std::runtime_error("the completion flip's lock taken while held");
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

struct PanicRestore {
    bool armed = false;
    bool panicking = false;
};

inline PanicRestore gPanicRestore;

inline std::function<int(const TegraScanoutCopy *)> gOffer;

// A unit whose window A WINBUF aperture (0xbc0..) refuses: kDropsWinbuf loses the stores, kParksWinbuf keeps them in a
// copy WIN_A_UPDATE does not take. Window A's DC_WIN aperture (0xb80..) lands either way.
struct DcModel {
    enum Aperture {
        kLands,
        kDropsWinbuf,
        kParksWinbuf
    };

    static constexpr uint32_t kStateAccess = 0x040, kStateControl = 0x041, kHeader = 0x042, kWindowASelect = 1u << 4;
    static constexpr uint32_t kStart = 0x800, kActReqs = 3u, kWinAUpdate = 1u << 9, kWinAActReq = 1u << 1;
    std::map<uint32_t, uint32_t> general, assembly, armed, active, parked;
    uint32_t requested = 0;
    bool clocked = true;
    Aperture winbuf = kLands;
    unsigned selectorWrites = 0, boundaries = 0;
    std::vector<uint32_t> written;
    std::function<void()> beforeWrite;  // another CPU's work landing just ahead of the next write

    static int aperture(uint32_t word)
    {
        if (word >= 0xb80 && word <= 0xb99)
            return static_cast<int>(0x700 + (word - 0xb80));
        if (word >= 0xbc0 && word <= 0xbf9)
            return static_cast<int>(0x800 + (word - 0xbc0));
        return -1;
    }

    static bool windowWord(uint32_t word)
    {
        return word >= 0x700 && word <= 0x839;
    }

    void write(uint32_t word, uint32_t value)
    {
        if (const auto before = std::exchange(beforeWrite, nullptr))
            before();
        if (!clocked)
            throw std::runtime_error("a write to DC A while it is unclocked");
        written.push_back(word);
        if (const int a = aperture(word); a >= 0) {
            if (a < static_cast<int>(kStart) || winbuf == kLands)
                assembly[static_cast<uint32_t>(a)] = value;
            else if (winbuf == kParksWinbuf)
                parked[static_cast<uint32_t>(a)] = value;
        } else if (windowWord(word)) {
            if (general[kHeader] != kWindowASelect)
                throw std::runtime_error("a window A word written with another window selected");
            assembly[word] = value;
        } else if (word == kStateControl) {
            if (value & kWinAUpdate)
                armed = assembly;
            requested |= value & kActReqs;
        } else {
            if (word == kStateAccess || word == kHeader)
                ++selectorWrites;
            general[word] = value;
        }
    }

    uint32_t read(uint32_t word)
    {
        if (word == kStateControl)
            return requested;
        if (const int a = aperture(word); a >= 0)
            return assembly[static_cast<uint32_t>(a)];
        if (windowWord(word))
            return (general[kStateAccess] & 1u) ? active[word] : assembly[word];
        return general[word];
    }

    void boundary()
    {
        ++boundaries;
        if (requested & kWinAActReq)
            active = armed;
        requested = 0;
    }
};

struct Words {
    DcModel *dc = nullptr;

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
        return {dc, word};
    }
};

struct Display {
    static constexpr int kNoBuffer = TegraScanoutRing::kNone, kSlot0 = TegraScanoutRing::kMaxBuffers;
    static constexpr const char *kBlockScanName = "row scan";
    static constexpr uint32_t kDcIntStatus = 0x037, kDcIntEnable = 0x039, kDcStateAccess = 0x040,
                              kDcStateControl = 0x041, kDcWindowHeader = 0x042, kDcRegActControl = 0x043;
    static constexpr uint32_t kWinOptions = 0x700, kWinLineStride = 0x70a, kWinbufStartAddr = 0x800,
                              kWinbufHOffset = 0x806, kWinbufUnderflow = 0x80a, kWinbufSurfaceKind = 0x80b,
                              kWinABase = 0xb80;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWindowASelect = 1u << 4,
                              kWinAActHcntrSel = 1u << 2;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kGeneralUpdate = 1u << 8, kWinAUpdate = 1u << 9;
    static constexpr uint32_t kWinAUnderflowInt = 1u << 8, kBlockScan = 0;
    static constexpr uint32_t kBlockSurfaceKind = 0x42, kBlockLineStride = 0x1800, kBlockHOffset = 0;
    static constexpr unsigned kLatchFrames = 4, kDirectUnderflowFrames = 3, kUnderflowFrames = 30;
    static constexpr uint64_t kProbeSlot = 0x10000000, kSlotSpan = 0x1000000;
    static constexpr uint64_t kBootFramebuffer = 0xc0000000;

    struct Plane {
        uint64_t seq = 0;
    };

    struct Mark {
        uint64_t ready = 0;
    };

    struct Probe {
        bool active() const
        {
            return false;
        }
    } probe;

    DcModel model;
    Words dc{&model};
    TegraScanoutRing ring;
    std::array<uint64_t, TegraScanoutRing::kMaxEntries> entrySeq{}, entryPendingSeq{};
    std::array<bool, TegraScanoutRing::kMaxEntries> entryPlaneSet{};
    std::array<Plane, TegraScanoutRing::kMaxEntries> entryPlane{};
    std::array<Mark, TegraScanoutRing::kMaxEntries> marks{};
    Plane planeNext, planeTarget;
    bool planeNextSet = false, rotated = true, rotationLatched = true, rotationAnnounced = true, doubleBuffer = true;
    bool directAttached = true, directOn = true, stats = false, probeUnderflow = false, probeRequest = false;
    bool hwSuspended = false, gpuDonePending = false, gpuScanout = false, transferred = false, deferred = false;
    bool copying = false, deferredCopy = false;
    int gpuStatus = 0, gpuDirect = kNoBuffer;
    uint32_t pitchActControl = 0, pitchOptions = 0x40000000;
    uint64_t frameEnds = 10, feStamp = 0, rotationRequested = 0, flipRequested = 0, flipRequest = 0, nextSwap = 40;
    uint64_t stReplaced = 0, stUnderflows = 0;
    unsigned underflowRun = 0, deadlineArms = 0, signals = 0;
    std::vector<std::pair<int, uint64_t>> shown;  // (entry, stamp) for every latch the ring saw
    std::function<void()> midTick;
    // The family's swap queue as retirement sees it: a latch retires through its frame, but not the swap whose
    // completion is still out (SwitchRetireDisplay.h retireThrough).
    bool latchRetire = true;
    std::vector<OriginalLegacySwapRequest> swaps;
    std::vector<uint64_t> retired;
    // As NativeLegacyDisplay's startHardware and allocations leave them: surface 0 is the boot framebuffer.
    IOMemoryMap bootMap{kBootFramebuffer};
    IOMemoryMap *bootMapping = &bootMap;
    uint64_t scanAddresses[TegraScanoutRing::kMaxBuffers] = {kBootFramebuffer, kBootFramebuffer + 0x1000000,
                                                             kBootFramebuffer + 0x2000000};

    Display()
    {
        gPreemptionOff = 0;
        if (completionFlipLock)
            completionFlipLock->held = false;
        completionFlip = {};
        completionUnverified = {kNoBuffer, 0};
        completionFlipsProven = completionFlipsRefused = completionFilterOff = false;
        ring.reset(3);
        model.general[DcModel::kHeader] = kWindowASelect;
        model.assembly[kWinbufStartAddr] = surfaceAddress(0);
        model.armed = model.active = model.assembly;
    }

    // Slots 0-2 are pitch surfaces of one stride; slot 3 is tiled.
    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const
    {
        const bool tiled = i < kSlot0 || i == kSlot0 + 3;
        kind = tiled ? kBlockSurfaceKind : 0;
        stride = tiled ? kBlockLineStride : 2944;
        hOffset = 0;
    }

    bool sameLayout(int a, int b) const
    {
        uint32_t ka = 0, sa = 0, ha = 0, kb = 0, sb = 0, hb = 0;
        layoutOf(a, ka, sa, ha);
        layoutOf(b, kb, sb, hb);
        return ka == kb && sa == sb && ha == hb;
    }

#include "../SwitchScanoutAddress.h"

    int surfaceIndex(uint32_t address) const
    {
        for (int i = 0; i < static_cast<int>(TegraScanoutRing::kMaxEntries); ++i)
            if (surfaceAddress(i) == address)
                return i;
        return kNoBuffer;
    }

    int activeEntry()
    {
        return surfaceIndex(model.active[kWinbufStartAddr]);
    }

    uint32_t expectedStart() const
    {
        return surfaceAddress(ring.pending != kNoBuffer ? ring.pending : ring.front);
    }

    uint64_t seqOf(uint64_t request) const
    {
        return request;
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *request) const
    {
        return request->seq;
    }

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return swaps.empty() ? nullptr : &swaps.front();
    }

    bool retireSwap(const OriginalLegacySwapRequest *request, bool)
    {
        if (request != pendingSwapRequest())
            throw std::runtime_error("a retirement out of order");
        retired.push_back(request->seq);
        swaps.erase(swaps.begin());
        return true;
    }

    void markFlip(int i)
    {
        marks.at(static_cast<size_t>(i)).ready = mach_absolute_time();
    }

    void checkPitchWindow()
    {}

    void chainPanicHook()
    {}

    void scanoutMemoryBarrier()
    {}

    void armFlipDeadline()
    {
        ++deadlineArms;
    }

    void planeStage(const Plane &, uint32_t &, uint32_t &)
    {
        throw std::runtime_error("no planes here");
    }

    uint32_t windowActive(uint32_t word)
    {
        return model.active[word];
    }

    void stopConsoleTape()
    {}

    void releaseIdleDirectReads()
    {}

    void notePresent(uint64_t, uint64_t, const Mark &)
    {}

    void retireThrough(uint64_t seq, uint64_t)
    {
        while (const auto *head = pendingSwapRequest()) {
            if (head->seq > seq || (gpuScanout && head->seq == flipRequest))
                break;
            retireSwap(head, true);
        }
    }

    static bool mediaFlipIdle()
    {
        return true;
    }

    void writeMediaFlipLocked(int)
    {}

    void takeMediaFlip()
    {}

    void mediaFlipFromGate()
    {}

    static void mediaFlipTotalsRegister()
    {}

    static int mediaFlipVerdict(int status)
    {
        return status;
    }

    static bool columnScanBusy()
    {
        return false;
    }

    // The UI window is A in this model (SwitchLandscapeUi.h's portrait case): no landscape, no window probe.
    Words::Ref uiWord(uint32_t index)
    {
        return dc[index];
    }

    uint32_t uiActive(uint32_t index)
    {
        return windowActive(index);
    }

    uint32_t uiUpdate() const
    {
        return kWinAUpdate;
    }

    uint32_t uiRequest() const
    {
        return kWinAActReq;
    }

    uint32_t uiUnderflow() const
    {
        return kWinAUnderflowInt;
    }

    char uiName() const
    {
        return 'A';
    }

    uint32_t underflowCounts[3] = {};

    void landscapeRunUnderflow(uint32_t)
    {}

    void noteLatchedUnderflow(uint32_t)
    {}

    void applyUiMemoryPlan()
    {}

    bool landscapeOn() const
    {
        return false;
    }

    bool landscapeHoldsKicks() const
    {
        return false;
    }

    static bool windowProbeBusy()
    {
        return false;
    }

    void flipLatched()
    {
        if (const int held = ring.flipHeld(); held != kNoBuffer)
            writeFlip(held);
    }

    // NativeLegacyDisplay's checkFlip, with every reading the ring cannot explain a failure.
    void checkFlip(bool atFrameEnd)
    {
        const auto [control, start] = latchState();
        const int asked = ring.pending;
        const int replaced = ring.superseded;
        switch (ring.latch(surfaceIndex(start), (control & (kGeneralActReq | kWinAActReq)) != 0u)) {
        case TegraScanoutRing::kWaiting:
            if (atFrameEnd && frameEnds - flipRequested >= kLatchFrames)
                throw std::runtime_error("window A never latched the flip");
            return;
        case TegraScanoutRing::kInconsistent:
            throw std::runtime_error("window A scans what the ring cannot explain");
        case TegraScanoutRing::kTaken:
            shown.emplace_back(asked, feStamp);
            retireThrough(entrySeq[asked], feStamp);
            break;
        case TegraScanoutRing::kTakenReplaced:
            shown.emplace_back(replaced, feStamp);
            retireThrough(entrySeq[replaced], feStamp);
            break;
        }
        flipLatched();
    }

    // NativeLegacyDisplay's offerDirect for slot entry, as far as the ring and window A see it.
    int offer(int entry)
    {
        TegraScanoutCopy copy = {};
        copy.display = this;
        const int offered = offerWithCompletionFlip(copy, entry);
        if (offered)
            return offered;
        gpuScanout = transferred = true;
        gpuDirect = entry;
        flipRequest = ++nextSwap;
        swaps.push_back({flipRequest});
        return 0;
    }

    // offerGpuCopy (and offerVicCopy, armed the same way): the frame is copied into the scan buffer ring.target().
    int offerCopy()
    {
        const int target = ring.target();
        if (target == kNoBuffer)
            return -1;
        TegraScanoutCopy copy = {};
        copy.display = this;
        const int offered = offerWithCompletionFlip(copy, target);
        if (offered)
            return offered;
        ring.claim(target);
        gpuScanout = transferred = copying = true;
        flipRequest = ++nextSwap;
        swaps.push_back({flipRequest});
        return 0;
    }

    // vicCopyWork once its job's fence is armed: the job's producer token is its syncpoint and fence.
    void armWaiter(uint32_t threshold)
    {
        armCompletionFlipWaiter(switch_gpu_backing_token{threshold, 1, SWITCH_GPU_ENGINE_VIC, 161});
    }

    // The GPU's nonstall filter with the watched producer complete and visible: interrupts off, its simple lock held.
    void gpuFilter()
    {
        ++gPreemptionOff;
        if (gGpuCallout)
            gGpuCallout(this, 0);
        --gPreemptionOff;
    }

    // host1x's syncpoint filter with the waiter's threshold reached, likewise; a pass retires the waiter.
    void vicFilter()
    {
        ++gPreemptionOff;
        if (auto *fn = gWaiterFn) {
            gWaiterFn = nullptr;
            fn(gWaiterContext);
        }
        --gPreemptionOff;
    }

    // gpuComplete, on the completion's thread.
    void complete(int status)
    {
        status = completionFlipVerdict(status);
        gpuStatus = status;
        gpuDonePending = true;
        writeCompletionFlip(status);
        ++signals;
    }

    // gpuDoneFired, under the gate.
    void donePass()
    {
        if (gpuDonePending) {
            gpuDonePending = false;
            finishGpuSwap(gpuStatus);
        }
    }

    // finishGpuCopy and presentGpuCopy: a direct slot is presented as it is, a copy through flip()'s ring.target().
    void finishGpuSwap(int status)
    {
        gpuScanout = false;
        if (copying) {
            copying = false;
            if (!status && hwSuspended) {
                deferredCopy = true;
                return;
            }
            if (status)
                ring.release();
            else if (!flippedByCompletion())
                presentEntry(ring.target());
            transferred = false;
            return;
        }
        const int entry = gpuDirect;
        gpuDirect = kNoBuffer;
        if (status) {
            transferred = false;
            return;
        }
        if (hwSuspended) {
            deferred = true;
            gpuDirect = entry;
            return;
        }
        presentEntry(entry);
        transferred = false;
    }

    // vblankTick's window A steps: the filter stamped the boundary; this runs at some later point.
    void tick()
    {
        takeCompletionFlip();
        if (midTick)
            midTick();
        frameEnds = frameEnds + 1;
        if (rotated)
            checkRotation();
    }

    void boundary()
    {
        model.boundary();
        feStamp = mach_absolute_time();
    }

    // Windows B and C as SwitchSuspendRetire.h reads them: off here.
    struct {
        int armed = -2, superseded = -2;
        bool armedRequested = true;
    } planeLatch;

    bool planesOn = false;

    Plane planeSetOf(int) const
    {
        return {};
    }

    // quiesceForSuspend and finishResume as far as window A sees them.
    void suspend()
    {
        suspendCompletionFlip();
        const uint64_t shown = suspendShownSeq();
        ring.settle();
        hwSuspended = true;
        model.clocked = false;
        retireSuspendShown(shown);
    }

    void resume()
    {
        model.clocked = true;
        hwSuspended = false;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        if (deferred) {
            deferred = false;
            const int entry = gpuDirect;
            gpuDirect = kNoBuffer;
            presentEntry(entry);
            transferred = false;
        }
        if (deferredCopy) {
            deferredCopy = false;
            if (!flippedByCompletion())
                presentEntry(ring.target());
            transferred = false;
        }
        if (const int held = ring.flipHeld(); held != kNoBuffer)
            writeFlip(held);
    }

#define SWITCH_FRAME_TIMELINE 0
#define KDBG(...)
#include "../SwitchScanoutStart.h"
#include "../SwitchCompletionFlipDisplay.h"
#include "../SwitchCompletionFlipFilter.h"
#include "../SwitchSuspendRetire.h"
#undef KDBG
#undef SWITCH_FRAME_TIMELINE
};

}  // namespace completion_flip_test

extern "C" inline int tegra_gm20b_scanout_copy(const TegraScanoutCopy *copy)
{
    return completion_flip_test::gOffer ? completion_flip_test::gOffer(copy) : 0;
}

extern "C" inline void tgm_present_filter_callout(void (*fn)(void *, int), void *)
{
    completion_flip_test::gGpuCallout = fn;
}

extern "C" inline int switch_host1x_filter_wait(uint32_t, uint32_t threshold, void (*fn)(void *), void *ctx,
                                                switch_host1x_filter_handle *handle)
{
    using namespace completion_flip_test;
    ++gWaits;
    *handle = threshold;
    if (gWaitResult == 0) {
        gWaiterFn = fn;
        gWaiterContext = ctx;
    }
    return gWaitResult;
}

extern "C" inline int switch_gpu_backing_token_filter_status(const switch_gpu_backing_token *token)
{
    completion_flip_test::gWaiterToken = *token;
    return completion_flip_test::gVicStatus;
}
