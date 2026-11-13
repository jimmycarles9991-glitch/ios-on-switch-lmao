// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_fence.h"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks;

void check(bool value)
{
    if (!value)
        std::abort();
    ++checks;
}

// The kernel publishes its newest failure alone: that job failed, the jobs after it did not, and one before it may have
// failed too without the record saying so any more (Superseded: the kernel's WaitFence answers).
void newestFailure()
{
    using S = raw::SemanticFenceStatus;
    TegraGM20BAbiFenceSemantic record{};
    check(raw::readSemanticFence(&record, 47).status == S::Clear);
    record = {2, 50, 0, TegraGM20BFenceSemanticPending};
    check(raw::readSemanticFence(&record, 49).status == S::Clear);
    check(raw::readSemanticFence(&record, 50).status == S::Pending);
    check(raw::readSemanticFence(&record, 51).status == S::Pending);
    record = {4, 50, 52, TegraGM20BFenceSemanticPending | TegraGM20BFenceSemanticFailed};
    check(raw::readSemanticFence(&record, 49).status == S::Superseded);
    check(raw::readSemanticFence(&record, 51).status == S::Pending);
    check(raw::readSemanticFence(&record, 52).status == S::Failed);
    check(raw::readSemanticFence(&record, 52).first == 52);
    check(raw::readSemanticFence(&record, 90).status == S::Pending);
    record = {6, 0, 52, TegraGM20BFenceSemanticFailed};
    check(raw::readSemanticFence(&record, 51).status == S::Superseded);
    check(raw::readSemanticFence(&record, 52).status == S::Failed);
    check(raw::readSemanticFence(&record, 53).status == S::Clear);
    check(raw::readSemanticFence(&record, 90).status == S::Clear);
    record = {8, 52, 52, 0};
    check(raw::readSemanticFence(&record, 90).status == S::Clear);
    record = {10, 0, 0, 4};
    check(raw::readSemanticFence(&record, 90).status == S::Pending);
}

void wrapping()
{
    using S = raw::SemanticFenceStatus;
    TegraGM20BAbiFenceSemantic record{0xfffffffe, 0xfffffffe, 2, TegraGM20BFenceSemanticPending};
    check(raw::readSemanticFence(&record, 0xfffffffd).status == S::Clear);
    check(raw::readSemanticFence(&record, 0xfffffffe).status == S::Pending);
    check(raw::readSemanticFence(&record, 0).status == S::Pending);
    check(raw::readSemanticFence(&record, 1).status == S::Pending);
    record.flags |= TegraGM20BFenceSemanticFailed;
    check(raw::readSemanticFence(&record, 0xfffffffd).status == S::Superseded);
    check(raw::readSemanticFence(&record, 1).status == S::Pending);
    check(raw::readSemanticFence(&record, 2).status == S::Failed);
    record = {0, 2, 0xfffffffe, TegraGM20BFenceSemanticFailed};
    check(raw::readSemanticFence(&record, 0xfffffffd).status == S::Superseded);
    check(raw::readSemanticFence(&record, 0xfffffffe).status == S::Failed);
    check(raw::readSemanticFence(&record, 0).status == S::Clear);
    check(raw::readSemanticFence(&record, 1).status == S::Clear);
    record = {2, 0, 0, TegraGM20BFenceSemanticPending};
    check(raw::readSemanticFence(&record, 0).status == S::Pending);
    record.flags = TegraGM20BFenceSemanticFailed;
    check(raw::readSemanticFence(&record, 0).status == S::Failed);
    check(raw::readSemanticFence(&record, 1).status == S::Clear);
}

void interruptedPublication()
{
    using S = raw::SemanticFenceStatus;
    std::array<uint32_t, 4> record{0, 0, 0, 0};
    unsigned reads = 0;
    auto result = raw::readSemanticFence(7, [&](unsigned field) {
        ++reads;
        return field == 0 ? 3u : record[field];
    });
    check(result.status == S::Pending && reads == 1);
    for (unsigned interrupted = 1; interrupted <= 4; ++interrupted) {
        record = {0xfffffffe, 0, 0, 0};
        reads = 0;
        result = raw::readSemanticFence(7, [&](unsigned field) {
            if (reads++ == interrupted)
                record = {0, 7, 7, TegraGM20BFenceSemanticFailed};
            return record[field];
        });
        check(result.status == S::Pending && reads == 5);
    }
    record = {2, 7, 7, TegraGM20BFenceSemanticFailed};
    result = raw::readSemanticFence(7, [&](unsigned field) {
        return record[field];
    });
    check(result.status == S::Failed && result.first == 7);
}
}

int main()
{
    newestFailure();
    wrapping();
    interruptedPublication();
    printf("mapped semantic fence: %u checks passed\n", checks);
}
