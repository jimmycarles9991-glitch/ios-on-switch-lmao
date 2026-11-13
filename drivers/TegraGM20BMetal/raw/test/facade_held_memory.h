// SPDX-License-Identifier: MIT
// facadecost's checks of the memory a command buffer's job reads while the mock's GPU holds it: a command buffer
// recorded behind it takes none of its setBytes chunks (MOCK_GPU_US), memory pressure's release gives back idle memory
// at once and none of what the job reads, and the idle release waits for it but not for one parked on an event.
#pragma once
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include "GpuMemoryRelease.h"

@interface NSObject (SwitchRawBytesChunks)
- (NSArray<NSNumber *> *)switchRawBytesChunks;  // RawMetalCommandBuffer.mm (debug builds)
@end

extern GpuMemoryRelease &gRawRelease;  // RawMetalFinish.mm: the raw device's, which memory pressure runs
extern "C" bool cap_read(uint64_t va, void *out, uint64_t bytes);          // the mock's view of GPU memory
extern "C" void mock_iokit_held(uint64_t *allocated, uint64_t *imported);  // the mock's live BOs' bytes
extern "C" void mock_iokit_pause(uint32_t channel, bool pause);            // a channel's jobs wait until unpaused

namespace facade_held {

// GpuMemoryRelease's idle second, its leeway and a margin
constexpr auto kPastIdleDeadline = std::chrono::milliseconds(1000 + 100 + 400);

inline std::set<uint64_t> bytesChunks(id<MTLCommandBuffer> cb)
{
    std::set<uint64_t> vas;
    for (NSNumber *va in [(id)cb switchRawBytesChunks])
        vas.insert(va.unsignedLongLongValue);
    return vas;
}

inline uint64_t heldBytes()
{
    uint64_t allocated = 0, imported = 0;
    mock_iokit_held(&allocated, &imported);
    return allocated;
}

// Every channel's jobs held on the mock GPU (or let go): a command buffer committed meanwhile stays in flight
inline void pauseGpu(bool pause)
{
    for (uint32_t channel = 0; channel < 16; channel++)
        mock_iokit_pause(channel, pause);
}

// `count` command buffers committed and completed: their memory idle, and the drain's idle deadline set
template <typename Encode> void settle(Encode encode, unsigned count)
{
    @autoreleasepool {
        NSMutableArray<id<MTLCommandBuffer>> *done = [NSMutableArray array];
        for (unsigned i = 0; i < count; i++)
            [done addObject:encode()];
        for (id<MTLCommandBuffer> cb in done)
            [cb waitUntilCompleted];
    }
}

// A command buffer recorded while an earlier one is in flight takes none of the earlier one's setBytes chunks (they come
// back only at its completion). The failures.
template <typename Encode> int heldChunkCheck(Encode encode)
{
    id<MTLCommandBuffer> first = encode();
    const std::set<uint64_t> held = bytesChunks(first);
    const bool inFlight = first.status != MTLCommandBufferStatusCompleted;
    id<MTLCommandBuffer> second = encode();
    unsigned shared = 0;
    for (uint64_t va : bytesChunks(second))
        shared += held.count(va) != 0;
    [first waitUntilCompleted];
    [second waitUntilCompleted];
    printf("facadecost held chunk: first in flight %d with %zu chunks, second shares %u of them\n", int(inFlight),
           held.size(), shared);
    if (inFlight && !held.empty() && !shared)
        return 0;
    printf("FAIL: the held-chunk check %s\n", !inFlight      ? "saw the first complete before the second"
                                              : held.empty() ? "found no chunk"
                                                             : "found a chunk taken while its job ran");
    return 1;
}

// Memory pressure's release while a command buffer is in flight (the GPU held), after `idle` command buffers completed
// and left their memory idle: it gives back some of that at once, and the in-flight one's setBytes chunks stay bound with
// their bytes until it completes, without an error. The failures.
template <typename Encode> int pressureCheck(Encode encode, unsigned idle)
{
    settle(encode, idle);
    pauseGpu(true);
    id<MTLCommandBuffer> cb = encode();
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> chunks;
    for (uint64_t va : bytesChunks(cb)) {
        std::vector<uint8_t> bytes(256);
        if (cap_read(va, bytes.data(), bytes.size()))
            chunks.emplace_back(va, std::move(bytes));
    }
    const uint64_t before = heldBytes();
    gRawRelease.release();
    const uint64_t after = heldBytes();
    const bool inFlight = cb.status != MTLCommandBufferStatusCompleted;
    size_t kept = 0;
    for (const auto &[va, bytes] : chunks) {
        std::vector<uint8_t> now(bytes.size());
        kept += cap_read(va, now.data(), now.size()) && now == bytes;
    }
    pauseGpu(false);
    [cb waitUntilCompleted];
    const bool completed = cb.status == MTLCommandBufferStatusCompleted && !cb.error;
    printf("facadecost pressure: in flight %d, gave back %llu KiB, %zu of %zu chunks bound with their bytes, completed "
           "%d\n",
           int(inFlight), (unsigned long long)(before > after ? (before - after) >> 10 : 0), kept, chunks.size(),
           int(completed));
    if (inFlight && after < before && !chunks.empty() && kept == chunks.size() && completed)
        return 0;
    printf("FAIL: the pressure check %s\n", !inFlight         ? "saw the command buffer complete before the release"
                                            : after >= before ? "saw nothing given back"
                                            : chunks.empty()  ? "found no chunk"
                                            : !completed      ? "saw the command buffer fail"
                                                              : "found a chunk of the job in flight freed or changed");
    return 1;
}

// The idle release with a command buffer in flight past its deadline (the GPU held): it gives back nothing then, and once
// the command buffer completes, at the deadline its drain set, it does. The failures.
template <typename Encode> int idleCheck(Encode encode, unsigned idle)
{
    settle(encode, idle);
    pauseGpu(true);
    id<MTLCommandBuffer> cb = encode();
    const uint64_t committed = heldBytes();
    std::this_thread::sleep_for(kPastIdleDeadline);
    const uint64_t inFlight = heldBytes();
    const bool held = cb.status != MTLCommandBufferStatusCompleted;
    pauseGpu(false);
    [cb waitUntilCompleted];
    std::this_thread::sleep_for(kPastIdleDeadline);
    const uint64_t drained = heldBytes();
    printf("facadecost idle: in flight %d past the deadline gave back %llu KiB, after its drain %llu KiB\n", int(held),
           (unsigned long long)(committed > inFlight ? (committed - inFlight) >> 10 : 0),
           (unsigned long long)(inFlight > drained ? (inFlight - drained) >> 10 : 0));
    if (held && inFlight >= committed && drained < inFlight)
        return 0;
    printf("FAIL: the idle check %s\n", !held ? "saw the command buffer complete while the GPU was held"
                                        : inFlight < committed ? "saw memory given back with a command buffer in flight"
                                                               : "saw nothing given back after the drain");
    return 1;
}

// The idle release with only a command buffer that waits on the CPU for an event nobody has signalled (no GPU work in
// flight): it gives back the idle memory at the deadline the last drain set, and the waiting command buffer's setBytes
// chunks stay bound with their bytes until the event comes and it completes. `encodeAfter(event)`: a command buffer that
// waits for `event` at 1. The failures.
template <typename EncodeAfter> int eventParkedCheck(EncodeAfter encodeAfter, id<MTLDevice> device, unsigned idle)
{
    settle(
        [&] {
            return encodeAfter(nil);
        },
        idle);
    id<MTLSharedEvent> event = [device newSharedEvent];
    id<MTLCommandBuffer> cb = encodeAfter(event);
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> chunks;
    for (uint64_t va : bytesChunks(cb)) {
        std::vector<uint8_t> bytes(256);
        if (cap_read(va, bytes.data(), bytes.size()))
            chunks.emplace_back(va, std::move(bytes));
    }
    const uint64_t parked = heldBytes();
    std::this_thread::sleep_for(kPastIdleDeadline);
    const uint64_t released = heldBytes();
    const bool waiting = cb.status != MTLCommandBufferStatusCompleted;
    size_t kept = 0;
    for (const auto &[va, bytes] : chunks) {
        std::vector<uint8_t> now(bytes.size());
        kept += cap_read(va, now.data(), now.size()) && now == bytes;
    }
    event.signaledValue = 1;
    [cb waitUntilCompleted];
    const bool completed = cb.status == MTLCommandBufferStatusCompleted && !cb.error;
    printf(
        "facadecost event-parked: waiting %d, gave back %llu KiB past the deadline, %zu of %zu chunks bound with their "
        "bytes, completed %d\n",
        int(waiting), (unsigned long long)(parked > released ? (parked - released) >> 10 : 0), kept, chunks.size(),
        int(completed));
    if (waiting && released < parked && !chunks.empty() && kept == chunks.size() && completed)
        return 0;
    printf("FAIL: the event-parked check %s\n",
           !waiting             ? "saw the command buffer run before its event"
           : released >= parked ? "saw nothing given back with only an event-parked command buffer"
           : chunks.empty()     ? "found no chunk"
           : !completed         ? "saw the command buffer fail"
                                : "found a chunk of the waiting command buffer freed or changed");
    return 1;
}

}  // namespace facade_held
