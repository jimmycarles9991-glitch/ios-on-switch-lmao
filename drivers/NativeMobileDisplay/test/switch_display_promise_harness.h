// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../TegraDisplayFlip/TegraScanoutRing.h"
#include "../../TegraGM20B/SwitchGpuBacking.h"
#include "../../TegraGM20B/SwitchGpuPresentWatch.h"
#include "../../TegraGM20B/SwitchGpuPromiseGiveUp.h"
#include "../../TegraGM20B/TegraGM20BJobABI.h"
#include "../../TegraHost1x/SwitchHost1xFence.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../SwitchCompletionFlip.h"

struct switch_gpu_backing {
    unsigned refs = 1, holds = 0;
    int promises = 0;
};

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
namespace switch_gpu_refusal {
constexpr bool logged(uint32_t nth)
{
    return nth < 4 || nth % 256 == 0;
}
}  // namespace switch_gpu_refusal
#endif

namespace display_promise_test {

constexpr uint32_t kPromiseOffset = 2052, kPromiseOwner = 7;

struct OriginalLegacySwapRequest {
    uint32_t swap = 0;
    uint64_t seq = 0;
};

constexpr IOReturn kIOReturnSuccess = 0, kIOReturnAborted = -14;

inline uint64_t gNowNs = 1000000000ULL;
inline std::vector<std::string> gLog;
inline void (*gGpuCallout)(void *, int) = nullptr;
inline void *gGpuContext = nullptr;
inline std::function<int(const TegraScanoutCopy *)> gOffer;
inline int gExternalStatus = 1;

inline struct PanicRestore {
    bool armed = false, panicking = false;
} gPanicRestore;

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    throw std::runtime_error(text);
}

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    gLog.emplace_back(text);
}

inline uint64_t mach_absolute_time()
{
    return gNowNs;
}

inline void switch_interrupt_describe(uint32_t, char *text, size_t size)
{
    snprintf(text, size, "GIC");
}

struct OSObject {};

struct IOInterruptEventSource {
    unsigned signals = 0;

    void interruptOccurred(void *, void *, int)
    {
        ++signals;
    }
};

struct IOTimerEventSource {
    unsigned arms = 0, cancels = 0;

    void setTimeoutMS(unsigned)
    {
        ++arms;
    }

    void cancelTimeout()
    {
        ++cancels;
    }
};

struct IOWorkLoop {
    template <class Action, class Owner> IOReturn runAction(Action action, Owner *owner, void *a0)
    {
        return action(owner, a0, nullptr, nullptr, nullptr);
    }
};

struct IOMemoryMap {
    uint64_t physical = 0;

    uint64_t getPhysicalAddress() const
    {
        return physical;
    }
};

struct DcModel {
    static constexpr uint32_t kIntStatus = 0x037, kStateAccess = 0x040, kStateControl = 0x041, kHeader = 0x042,
                              kWindowASelect = 1u << 4, kActReqs = 3u, kWinAUpdate = 1u << 9, kWinAActReq = 1u << 1;
    std::map<uint32_t, uint32_t> general, assembly, armed, active;
    uint32_t requested = 0;
    std::vector<uint32_t> written;

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
        written.push_back(word);
        if (word == kIntStatus) {
            general[word] &= ~value;
            return;
        }
        if (const int a = aperture(word); a >= 0) {
            assembly[static_cast<uint32_t>(a)] = value;
        } else if (windowWord(word)) {
            assembly[word] = value;
        } else if (word == kStateControl) {
            if (value & kWinAUpdate)
                armed = assembly;
            requested |= value & kActReqs;
        } else {
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

struct Display : OSObject {
    static constexpr int kNoBuffer = TegraScanoutRing::kNone, kSlot0 = TegraScanoutRing::kMaxBuffers;
    static constexpr unsigned kSlots = TegraScanoutRing::kMaxEntries - TegraScanoutRing::kMaxBuffers;
    static constexpr const char *kBlockScanName = "row scan";
    static constexpr uint32_t kDcIntStatus = 0x037, kDcIntMask = 0x038, kDcIntEnable = 0x039, kDcStateAccess = 0x040,
                              kDcStateControl = 0x041, kDcWindowHeader = 0x042, kDcRegActControl = 0x043;
    static constexpr uint32_t kWinOptions = 0x700, kWinLineStride = 0x70a, kWinbufStartAddr = 0x800,
                              kWinbufHOffset = 0x806, kWinbufUnderflow = 0x80a, kWinbufSurfaceKind = 0x80b,
                              kWinABase = 0xb80;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWindowASelect = 1u << 4,
                              kWinAActHcntrSel = 1u << 2;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kGeneralUpdate = 1u << 8, kWinAUpdate = 1u << 9;
    static constexpr uint32_t kWinAUnderflowInt = 1u << 8, kFrameEndInt = 1u << 1, kBlockScan = 0;
    static constexpr uint32_t kBlockSurfaceKind = 0x42, kBlockLineStride = 0x1800, kBlockHOffset = 0;
    static constexpr unsigned kLatchFrames = 4, kDirectUnderflowFrames = 3, kUnderflowFrames = 30;
    static constexpr unsigned kFlipDeadlineMS = 100, kGpuCopyDeadlineMS = 10000;
    static constexpr uint64_t kProbeSlot = 0x10000000, kSlotSpan = 0x1000000, kBootFramebuffer = 0xc0000000;
    static constexpr uint32_t kDisplayAInterrupt = 105;
    static constexpr size_t swapId = offsetof(OriginalLegacySwapRequest, swap);

    struct Plane {
        uint64_t seq = 0;
        bool fenced = false;
    };

    struct Mark {
        uint64_t ready = 0;
    };

    struct Probe {
        bool onIova = false;

        bool active() const
        {
            return false;
        }
    } probe;

    struct DirectSlot {
        SwitchNativeScanout *read = nullptr;
        uint64_t used = 0;
    };

    struct {
        static bool requested()
        {
            return false;
        }
    } planeLatch;

    DcModel model;
    Words dc{&model};
    TegraScanoutRing ring;
    DirectSlot slots[kSlots]{};
    SwitchNativeScanout reads[kSlots]{};
    std::array<uint64_t, TegraScanoutRing::kMaxEntries> entrySeq{}, entryPendingSeq{};
    std::array<bool, TegraScanoutRing::kMaxEntries> entryPlaneSet{};
    std::array<Plane, TegraScanoutRing::kMaxEntries> entryPlane{};
    std::array<Mark, TegraScanoutRing::kMaxEntries> marks{};
    Plane planeNext{}, planeTarget{};
    bool planeNextSet = false, rotated = true, rotationLatched = true, rotationAnnounced = true, doubleBuffer = true;
    bool directAttached = true, directOn = true, stats = false, probeUnderflow = false, probeRequest = false;
    bool hwSuspended = false, gpuDonePending = false, gpuScanout = false, transferred = false, latchRetire = true;
    bool copying = false, scanoutStopped = false, settleArmed = false, planesOn = false;
    int gpuStatus = 0, gpuDirect = kNoBuffer, probeSurface = 0;
    uint32_t pitchActControl = 0, pitchOptions = 0x40000000, stDirect = 0, underflowCounts[3] = {};
    unsigned underflowRun = 0, donePasses = 0, observed = 0, unshownRetired = 0;
    uint64_t frameEnds = 10, feStamp = 0, rotationRequested = 0, flipRequested = 0, nextSwap = 40;
    uint64_t stReplaced = 0, stUnderflows = 0, gpuDoneAt = 0, gpuSequence = 0, gpuLength = 0;
    const OriginalLegacySwapRequest *flipRequest = nullptr, *gpuRequest = nullptr;
    std::deque<OriginalLegacySwapRequest> swaps;
    std::vector<uint32_t> shownSwaps, unshownSwaps;
    std::vector<int> shownEntries;
    IOInterruptEventSource gpuDoneStore;
    IOInterruptEventSource *gpuDoneSource = &gpuDoneStore;
    IOTimerEventSource flipTimerStore;
    IOTimerEventSource *flipTimer = &flipTimerStore;
    IOWorkLoop loop;
    IOMemoryMap bootMap{kBootFramebuffer};
    IOMemoryMap *bootMapping = &bootMap;
    uint64_t scanAddresses[TegraScanoutRing::kMaxBuffers] = {kBootFramebuffer, kBootFramebuffer + 0x1000000,
                                                             kBootFramebuffer + 0x2000000};

    Display()
    {
        completionFlip = {};
        completionFlipOwner = nullptr;
        flipDeadlineArmed = offerAged = false;
        flipDeadlineFrames = gpuOfferDeadline = 0;
        completionPassFolds = kickWaiting = false;
        completionUnverified = {kNoBuffer, 0};
        ring.reset(3);
        model.general[DcModel::kHeader] = kWindowASelect;
        model.assembly[kWinbufStartAddr] = surfaceAddress(0);
        model.armed = model.active = model.assembly;
        for (unsigned k = 0; k < kSlots; ++k)
            slots[k].read = &reads[k];
    }

    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const
    {
        const bool tiled = i < kSlot0;
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

    uint64_t seqOf(const OriginalLegacySwapRequest *request) const
    {
        return request ? request->seq : 0;
    }

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return swaps.empty() ? nullptr : &swaps.front();
    }

    const OriginalLegacySwapRequest *queuedAfter(const OriginalLegacySwapRequest *queued) const
    {
        for (size_t i = 0; i + 1 < swaps.size(); ++i)
            if (&swaps[i] == queued)
                return &swaps[i + 1];
        return nullptr;
    }

    bool retireSwap(const OriginalLegacySwapRequest *request, bool)
    {
        if (request != pendingSwapRequest())
            throw std::runtime_error("out-of-order retirement");
        shownSwaps.push_back(request->swap);
        swaps.pop_front();
        return true;
    }

    void retireThrough(uint64_t seq, uint64_t)
    {
        while (const auto *head = pendingSwapRequest()) {
            if (head->seq > seq || (gpuScanout && head == gpuRequest))
                break;
            retireSwap(head, true);
        }
    }

    void retireSwapUnshown(const OriginalLegacySwapRequest *request, int directEntry)
    {
        if (directEntry == kNoBuffer)
            ring.release();
        if (request) {
            ++unshownRetired;
            unshownSwaps.push_back(request->swap);
            if (request == pendingSwapRequest())
                swaps.pop_front();
        }
        transferred = false;
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {}

    void native_legacy_transfer_observed(IOReturn, const OriginalLegacySwapRequest *, uint64_t)
    {
        ++observed;
    }

    void markFlip(int i)
    {
        marks.at(static_cast<size_t>(i)).ready = mach_absolute_time();
    }

    uint64_t planeNextAlsoWritten() const
    {
        return 0;
    }

    bool planeFenced(const Plane &p) const
    {
        return p.fenced;
    }

    IOWorkLoop *getWorkLoop()
    {
        return &loop;
    }

    uint64_t gpuDeadlineTicks() const
    {
        return static_cast<uint64_t>(kGpuCopyDeadlineMS) * 1000000ULL;
    }

    [[noreturn]] void gpuDeadlineCheck()
    {
        panic("SwitchDisplay: GPU present not completed in %u ms", kGpuCopyDeadlineMS);
    }

    void checkPitchWindow()
    {}

    void chainPanicHook()
    {}

    void scanoutMemoryBarrier()
    {}

    void planeStage(const Plane &, uint32_t &, uint32_t &)
    {}

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

    void settleStopped()
    {}

    static bool frameEndPending(uint32_t)
    {
        return false;
    }

    static bool flipInterruptsOff()
    {
        return true;
    }

    static void flipInterruptsOn(bool)
    {}

    static void notePass()
    {}

    static bool planeRecoveryWaits()
    {
        return false;
    }

    static bool mediaFlipIdle()
    {
        return true;
    }

    void writeMediaFlip(int)
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

    static bool windowProbeBusy()
    {
        return false;
    }

    static bool host1xFlipBusy()
    {
        return false;
    }

    static bool dcChannelProbed()
    {
        return false;
    }

    bool landscapeOn() const
    {
        return false;
    }

    bool landscapeHoldsKicks() const
    {
        return false;
    }

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

    void landscapeRunUnderflow(uint32_t)
    {}

    void noteLatchedUnderflow(uint32_t)
    {}

    void applyUiMemoryPlan()
    {}

    void flipLatched()
    {
        if (const int held = ring.flipHeld(); held != kNoBuffer)
            writeFlip(held);
    }

    void checkFlip(bool atFrameEnd)
    {
        const auto [control, start] = latchState();
        const int asked = ring.pending, replaced = ring.superseded;
        switch (ring.latch(surfaceIndex(start), (control & (kGeneralActReq | kWinAActReq)) != 0u)) {
        case TegraScanoutRing::kWaiting:
            if (atFrameEnd && frameEnds - flipRequested >= kLatchFrames)
                throw std::runtime_error("window A never latched the flip");
            return;
        case TegraScanoutRing::kInconsistent:
            throw std::runtime_error("window A scans what the ring cannot explain");
        case TegraScanoutRing::kTaken:
            shownEntries.push_back(asked);
            retireThrough(entrySeq[asked], feStamp);
            break;
        case TegraScanoutRing::kTakenReplaced:
            shownEntries.push_back(replaced);
            retireThrough(entrySeq[replaced], feStamp);
            break;
        }
        flipLatched();
    }

    void finishGpuSwap(int status)
    {
        const OriginalLegacySwapRequest *request = gpuRequest;
        cancelGpuDeadline();
        gpuScanout = false;
        gpuRequest = nullptr;
        if (copying) {
            copying = false;
            if (acceptedUnshown(status, request, "GPU copy")) {
                retireSwapUnshown(request, kNoBuffer);
                return;
            }
            flipRequest = request;
            if (!flippedByCompletion())
                presentEntry(ring.target());
            transferred = false;
            return;
        }
        const int entry = gpuDirect;
        gpuDirect = kNoBuffer;
        if (acceptedUnshown(status, request, "direct")) {
            retireSwapUnshown(request, entry);
            return;
        }
        flipRequest = request;
        presentEntry(entry);
        transferred = false;
    }

    const OriginalLegacySwapRequest *enqueueSwap()
    {
        const uint64_t seq = ++nextSwap;
        swaps.push_back({static_cast<uint32_t>(seq), seq});
        return &swaps.back();
    }

    bool offerDirectSwap(int entry, const OriginalLegacySwapRequest *request)
    {
        if (presentDirectVisible(request, entry, 1280, 720, mach_absolute_time()))
            return true;
        TegraScanoutCopy copy = {};
        copy.version = TEGRA_SCANOUT_COPY_VERSION;
        copy.sequence = gpuSequence + 1;
        copy.display = this;
        copy.flags = TEGRA_SCANOUT_COPY_FENCE_ONLY | producerWatch(entry);
        const int offered = offerWithCompletionFlip(copy, entry);
        if (offered) {
            if (sourceUnshown(offered, request, "direct")) {
                gpuSequence = copy.sequence;
                planeNextSet = false;
                native_legacy_transfer_observed(kIOReturnAborted, request, 0);
                retireSwapUnshown(request, entry);
                return true;
            }
            return false;
        }
        gpuSequence = copy.sequence;
        gpuScanout = true;
        armGpuDeadline();
        gpuRequest = request;
        gpuDirect = entry;
        transferred = true;
        return true;
    }

    bool offerCopySwap(const OriginalLegacySwapRequest *request)
    {
        const int target = ring.target();
        if (target == kNoBuffer)
            return false;
        TegraScanoutCopy copy = {};
        copy.version = TEGRA_SCANOUT_COPY_VERSION;
        copy.sequence = gpuSequence + 1;
        copy.display = this;
        const int offered = offerWithCompletionFlip(copy, target);
        if (offered) {
            if (sourceUnshown(offered, request, "GPU copy")) {
                gpuSequence = copy.sequence;
                retireSwapUnshown(request, kNoBuffer);
                return true;
            }
            return false;
        }
        ring.claim(target);
        gpuSequence = copy.sequence;
        gpuScanout = copying = transferred = true;
        armGpuDeadline();
        gpuRequest = request;
        return true;
    }

    void gpuFilter()
    {
        if (gGpuCallout)
            gGpuCallout(gGpuContext, 0);
    }

    void donePass()
    {
        if (gpuDoneStore.signals == donePasses)
            return;
        donePasses = gpuDoneStore.signals;
        gpuDoneFired<Display *>(this, &gpuDoneStore, 0);
    }

    void boundaryAndTick()
    {
        model.boundary();
        feStamp = mach_absolute_time();
        takeCompletionFlip();
        frameEnds = frameEnds + 1;
        if (rotated)
            checkRotation();
        checkOfferAge();
    }

#define SWITCH_FRAME_TIMELINE 0
#define KDBG(...)
#include "../SwitchDirectVisibleDisplay.h"
#include "../SwitchUnshownSource.h"
#include "../SwitchScanoutStart.h"
#include "../SwitchCompletionFlipDisplay.h"
#include "../SwitchCompletionFlipFilter.h"
#include "../SwitchGpuDoneDisplay.h"
#include "../SwitchFlipDeadlineDisplay.h"
#undef KDBG
#undef SWITCH_FRAME_TIMELINE
};

}  // namespace display_promise_test

#include "switch_display_promise_kernel.h"

extern "C" inline void switch_gpu_backing_retain(switch_gpu_backing *b)
{
    ++b->refs;
}

extern "C" inline void switch_gpu_backing_release(switch_gpu_backing *b)
{
    --b->refs;
}

extern "C" inline void switch_gpu_backing_scanout(switch_gpu_backing *b, int add)
{
    add ? ++b->holds : --b->holds;
}

extern "C" inline int switch_gpu_backing_promised(switch_gpu_backing *b)
{
    return b ? b->promises : 0;
}

extern "C" inline int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *)
{
    return display_promise_test::gExternalStatus;
}

extern "C" inline int switch_gpu_backing_tokens_failure(const switch_gpu_backing_token *, unsigned)
{
    return 0;
}

extern "C" inline int tegra_gm20b_scanout_copy(const TegraScanoutCopy *copy)
{
    return display_promise_test::gOffer ? display_promise_test::gOffer(copy) : 0;
}

extern "C" inline void tgm_present_filter_owner(const void *)
{}

extern "C" inline void tgm_present_filter_callout(void (*fn)(void *, int), void *ctx)
{
    display_promise_test::gGpuCallout = fn;
    display_promise_test::gGpuContext = ctx;
}

extern "C" inline int switch_host1x_filter_wait(uint32_t, uint32_t, void (*)(void *), void *,
                                                switch_host1x_filter_handle *)
{
    return -1;
}

extern "C" inline int switch_gpu_backing_token_filter_status(const switch_gpu_backing_token *)
{
    return 1;
}
