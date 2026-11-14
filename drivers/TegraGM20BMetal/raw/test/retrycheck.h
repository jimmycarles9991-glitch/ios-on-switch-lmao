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
    bool failedAlone = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto failed = build.make(produce, place);
        failedAlone &= !failed.state && failed.retry && placements == i + 1;
    }
    expect(failedAlone && mock_iokit_calls(TegraGM20BMethodBoImport) - imports == 4,
           "each make that meets a program-backing allocation failure fails alone and the next places afresh");
    const auto recovered = build.make(produce, place);
    expect(recovered.state && !recovered.retry && placements == 5,
           "program placement succeeds once backing allocation is available");
    expect(produces == 1 && sameRecord, "a failed placement keeps the compiled record and its placement identity");
    const auto before = mock_iokit_calls(TegraGM20BMethodBoImport);
    const auto warm = build.make(produce, place);
    expect(warm.state == recovered.state && placements == 5 && mock_iokit_calls(TegraGM20BMethodBoImport) == before,
           "a ready compute state needs no compilation or placement");

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
    bool scratchFailed = true, heapPreserved = true;
    for (unsigned i = 0; i < 4; ++i) {
        const auto result = scratch.make(scratchRecord, [&](const ComputeRecord &record) {
            return placeCompute(device, record);
        });
        scratchFailed &= !result.state && result.retry;
        heapPreserved &= device.heapPlaced_ == heapAt;
    }
    expect(scratchFailed && mock_iokit_calls(TegraGM20BMethodBoAlloc) - allocations == 4,
           "each make that meets a scratch allocation failure fails alone");
    expect(heapPreserved, "failed scratch growth never consumes program heap slots");
    const auto scratchReady = scratch.make(scratchRecord, [&](const ComputeRecord &record) {
        return placeCompute(device, record);
    });
    expect(scratchReady.state && !scratchReady.retry && scratchProduces == 1 && device.heapPlaced_ > heapAt,
           "a later make places the retained program once");
}
}  // namespace raw
