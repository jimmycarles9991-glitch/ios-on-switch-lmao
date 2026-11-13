// SPDX-License-Identifier: MIT
// A raw command buffer's GPU time, Metal's GPUStartTime and GPUEndTime: two 16-byte host semaphore releases in its own push
// (NV906F SEMAPHORED, RELEASE_WFI EN, as the kernel's stats stamps), the first ahead of its prologue and the last after its final
// work, each writing the payload and PTIMER. The last one's wait for idle is the one the kernel's fence release makes right after
// it (drivers/TegraGM20B/gpu_exec.c tgm_chan_exec: WFI, the L2 flush, then the fence with RELEASE_WFI), so it holds the GPU no longer.
//
// PTIMER does not count nanoseconds here: its source is the 19.2 MHz oscillator and each tick adds 32, a step that assumes
// 31.25 MHz, so one unit is 31.25 / 19.2 = 625/384 ns. GK20A-family PTIMERs have no rate registers to set it right (nouveau's
// gk20a timer has no init; nvgpu scales by ptimer_src_freq), which is why the conversion lives with every reader.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"

namespace raw {

enum : uint32_t {
    HOST_SEMAPHORE_A = NVB06F_SEMAPHOREA,
    HOST_SEMAPHORE_RELEASE_16_WFI = NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) |
        NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN) | NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 16BYTE),
};

constexpr uint32_t kStampWords = 5;
constexpr uint64_t kPtimerNsNum = 625, kPtimerNsDen = 384;

struct StampRelease {
    uint32_t payload, pad;
    uint64_t ptimer;
};

struct StampSlot {
    StampRelease start, end;
};

constexpr uint32_t kStampSlots = 0x1000 / sizeof(StampSlot);

// A command buffer's stamp: never 0 (none), and a slot's stamps differ for as long as kStampSlots command buffers are in flight
inline uint32_t stampOf(uint64_t commits)
{
    return uint32_t(commits % 0xffffffffu) + 1;
}

inline uint64_t stampOffset(uint32_t stamp)
{
    return uint64_t((stamp - 1) % kStampSlots) * sizeof(StampSlot);
}

inline void emitStamp(Push &p, uint64_t va, uint32_t stamp)
{
    p.inc(HOST_SEMAPHORE_A, {uint32_t(va >> 32) & 0xff, uint32_t(va), stamp, HOST_SEMAPHORE_RELEASE_16_WFI});
}

// A command buffer's two releases into its slot of the stamp page at `slots`: the first ahead of its prologue, the last
// after its final work
inline void emitStartStamp(Push &p, uint64_t slots, uint32_t stamp)
{
    emitStamp(p, slots + stampOffset(stamp) + offsetof(StampSlot, start), stamp);
}

inline void emitEndStamp(Push &p, uint64_t slots, uint32_t stamp)
{
    emitStamp(p, slots + stampOffset(stamp) + offsetof(StampSlot, end), stamp);
}

// A measured draw's stamps (raw_band_rates.h), after the command buffers' slots in the same page set: 3D report semaphore
// releases at the end of the pipeline, after every earlier draw's writes, with PTIMER, so neither stalls the front end as a
// wait for idle would. The tiled cache is flushed ahead of each, or a binned draw's pixels could land after its stamp.
constexpr uint32_t kDrawStampSlots = 256;
constexpr uint64_t kDrawStampBase = kStampSlots * sizeof(StampSlot);
// After the measured draws' stamps, a record per command buffer slot: the stamp of the command buffer one of whose
// predicated loops would run past its copies, and that loop's tag (raw_predication.h endLoop)
constexpr uint64_t kLoopWordsBase = kDrawStampBase + kDrawStampSlots * sizeof(StampSlot);
constexpr uint64_t kStampBytes = kLoopWordsBase + kStampSlots * 8;
constexpr uint32_t kDrawStampWords = 5;
constexpr uint32_t kDrawStampRelease =
    NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
    NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, RELEASE, AFTER_ALL_PRECEEDING_WRITES_COMPLETE) |
    NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
    NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS) |
    NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, FLUSH_DISABLE, TRUE);

inline uint64_t drawStampOffset(uint32_t slot, bool end)
{
    return kDrawStampBase + uint64_t(slot % kDrawStampSlots) * sizeof(StampSlot) +
           (end ? offsetof(StampSlot, end) : offsetof(StampSlot, start));
}

inline uint64_t loopRecordOffset(uint32_t stamp)
{
    return kLoopWordsBase + uint64_t((stamp - 1) % kStampSlots) * 8;
}

inline void emitDrawStamp(Push &p, uint64_t stampsVa, uint32_t slot, bool end, uint32_t payload)
{
    const uint64_t va = stampsVa + drawStampOffset(slot, end);
    p.inc(NVB197_SET_REPORT_SEMAPHORE_A, {uint32_t(va >> 32) & 0xff, uint32_t(va), payload, kDrawStampRelease});
}

inline uint64_t ptimerNanoseconds(uint64_t units)
{
    return units / kPtimerNsDen * kPtimerNsNum + units % kPtimerNsDen * kPtimerNsNum / kPtimerNsDen;
}

// The GPU time between the slot's releases, when both are this stamp's (a later command buffer reusing the slot changes them)
inline bool stampedNanoseconds(const volatile StampSlot &slot, uint32_t stamp, uint64_t &ns)
{
    if (!stamp || slot.start.payload != stamp || slot.end.payload != stamp)
        return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t start = slot.start.ptimer, end = slot.end.ptimer;
    std::atomic_thread_fence(std::memory_order_acquire);
    // a later command buffer's start release may land on the slot while its times are read
    if (slot.start.payload != stamp || end < start)
        return false;
    ns = ptimerNanoseconds(end - start);
    return true;
}

// Metal's times of a command buffer, in its order: kernelStart <= kernelEnd <= GPUStart <= GPUEnd. The end is where the
// completion was seen and a stamped start the GPU's time before it, so a start can fall before the host saw the submission
// return: the kernel times give way to it.
struct CommandBufferTimes {
    double kernelStart, kernelEnd, gpuStart, gpuEnd;
};

inline CommandBufferTimes orderedTimes(double committed, double submitted, double completed, bool stamped,
                                       uint64_t gpuNs)
{
    CommandBufferTimes t;
    t.gpuEnd = completed;
    t.gpuStart = stamped ? completed - double(gpuNs) * 1e-9 : std::min(submitted, completed);
    t.kernelEnd = std::min(submitted, t.gpuStart);
    t.kernelStart = std::min(committed, t.kernelEnd);
    return t;
}

}  // namespace raw
