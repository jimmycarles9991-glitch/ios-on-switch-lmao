// SPDX-License-Identifier: MIT
// A raw command buffer's GPU time, Metal's GPUStartTime and GPUEndTime: two 16-byte host semaphore releases in its own push
// (NV906F SEMAPHORED, RELEASE_WFI EN, as the kernel's stats stamps), the first ahead of its prologue and the last after its final
// work, each writing the payload and PTIMER. The last one's wait for idle is the one the kernel's fence release makes right after
// it (drivers/TegraGM20B/gpu.c tgm_chan_exec: WFI, the L2 flush, then the fence with RELEASE_WFI), so it holds the GPU no longer.
//
// PTIMER does not count nanoseconds here: its source is the 19.2 MHz oscillator and each tick adds 32, a step that assumes
// 31.25 MHz, so one unit is 31.25 / 19.2 = 625/384 ns. GK20A-family PTIMERs have no rate registers to set it right (nouveau's
// gk20a timer has no init; nvgpu scales by ptimer_src_freq), which is why the conversion lives with every reader.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "raw_push.h"

namespace raw {

enum : uint32_t {
    HOST_SEMAPHORE_A = 0x0010,
    HOST_SEMAPHORE_RELEASE_16_WFI = 0x2,  // OPERATION RELEASE, RELEASE_WFI EN (0), RELEASE_SIZE 16BYTE (0)
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
