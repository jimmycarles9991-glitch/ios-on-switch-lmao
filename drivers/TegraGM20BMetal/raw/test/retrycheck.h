// SPDX-License-Identifier: MIT
#pragma once
#include "wrapcheck.h"
#include "../runtime/raw_compute.h"
#include "../runtime/raw_compute_lookup.h"

extern "C" void mock_iokit_fail_after(unsigned method, unsigned skip, unsigned count, int kr);
extern "C" unsigned long mock_iokit_calls(unsigned method);

namespace raw {
template <class Expect> void DeviceHarness::computeRetry(Device &device, Expect expect)
{
    using Build = ComputeKernelBuild<ComputeRecord, ComputeState>;
    Build build;
    uint64_t now = 1;
    unsigned produces = 0, placements = 0;
    const ComputeRecord *identity = nullptr;
    const auto produce = [&](ComputeRecord &record) {
        ++produces;
        record.code.resize(256, 0);
        record.kernel.regionWord = -1;
        identity = &record;
        return Build::Outcome::Made;
    };
    bool sameRecord = true;
    const auto place = [&](const ComputeRecord &record) {
        ++placements;
        sameRecord &= identity == &record;
        return placeCompute(device, record);
    };
    device.heapPlaced_ = device.heapBound_;
    mock_iokit_fail_after(TegraGM20BMethodBoImport, 0, 4, kIOReturnNoMemory);
    const auto imports = mock_iokit_calls(TegraGM20BMethodBoImport);
    bool retryable = true, throttled = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto failed = build.make(
            [&] {
                return now;
            },
            produce, place);
        retryable &= !failed.state && failed.retry && failed.retryAtUs > now;
        const auto again = build.make(
            [&] {
                return now;
            },
            produce, place);
        throttled &= !again.state && again.retry && placements == i + 1;
        now = failed.retryAtUs;
    }
    expect(retryable && mock_iokit_calls(TegraGM20BMethodBoImport) - imports == 4,
           "four real program-backing allocation failures remain transient");
    expect(throttled, "allocation pressure cannot launch placement again before its retry notification");
    const auto recovered = build.make(
        [&] {
            return now;
        },
        produce, place);
    expect(recovered.state && !recovered.retry && placements == 5,
           "program placement recovers when backing allocation becomes available");
    expect(produces == 1 && sameRecord,
           "allocation retries retain the original compiled record and its placement identity");
    const auto before = mock_iokit_calls(TegraGM20BMethodBoImport);
    const auto warm = build.make(
        []() -> uint64_t {
            std::abort();
        },
        produce, place);
    expect(warm.state == recovered.state && placements == 5 && mock_iokit_calls(TegraGM20BMethodBoImport) == before,
           "ready compute state needs no retry clock, compilation or placement");

    Build scratch;
    unsigned scratchProduces = 0;
    const auto scratchRecord = [&](ComputeRecord &record) {
        ++scratchProduces;
        record.code.resize(256, 0);
        record.kernel.qmd[45] = uint32_t(device.slmPerWarp_ / 32 + 16);
        return Build::Outcome::Made;
    };
    const auto heapAt = device.heapPlaced_;
    const auto allocations = mock_iokit_calls(TegraGM20BMethodBoAlloc);
    mock_iokit_fail_after(TegraGM20BMethodBoAlloc, 0, 4, kIOReturnNoMemory);
    bool scratchRetry = true, heapPreserved = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto result = scratch.make(
            [&] {
                return now;
            },
            scratchRecord,
            [&](const ComputeRecord &record) {
                return placeCompute(device, record);
            });
        scratchRetry &= !result.state && result.retry;
        heapPreserved &= device.heapPlaced_ == heapAt;
        now = result.retryAtUs;
    }
    expect(scratchRetry && mock_iokit_calls(TegraGM20BMethodBoAlloc) - allocations == 4,
           "four real scratch allocation failures remain transient");
    expect(heapPreserved, "failed scratch growth never consumes program heap slots");
    const auto scratchReady = scratch.make(
        [&] {
            return now;
        },
        scratchRecord,
        [&](const ComputeRecord &record) {
            return placeCompute(device, record);
        });
    expect(scratchReady.state && !scratchReady.retry && scratchProduces == 1 && device.heapPlaced_ > heapAt,
           "scratch recovery places the retained program once");
}
}  // namespace raw
