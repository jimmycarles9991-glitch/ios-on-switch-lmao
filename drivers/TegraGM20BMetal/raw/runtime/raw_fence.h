// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BABI.h"
#include <atomic>
#include <cstdint>

namespace raw {

// Superseded: physically complete, and the record names a newer failure, so it cannot say whether this job failed too
// (the kernel publishes only its newest); the kernel's WaitFence answers that.
enum class SemanticFenceStatus : uint8_t {
    Clear,
    Pending,
    Failed,
    Superseded
};

struct SemanticFenceRead {
    SemanticFenceStatus status = SemanticFenceStatus::Clear;
    uint32_t first = 0;
};

template <class Read> SemanticFenceRead readSemanticFence(uint32_t seqno, Read read)
{
    const uint32_t before = read(0);
    if (before & 1)
        return {SemanticFenceStatus::Pending, 0};
    const uint32_t pending = read(1), failed = read(2), flags = read(3);
    // Pair the kernel's odd/fields/even publication without spinning on its writer.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (read(0) != before || flags & ~(TegraGM20BFenceSemanticPending | TegraGM20BFenceSemanticFailed))
        return {SemanticFenceStatus::Pending, 0};
    // A failure is its own job's alone: the jobs after it do not depend on its wait.
    if ((flags & TegraGM20BFenceSemanticFailed) && seqno == failed)
        return {SemanticFenceStatus::Failed, failed};
    if ((flags & TegraGM20BFenceSemanticPending) && int32_t(seqno - pending) >= 0)
        return {SemanticFenceStatus::Pending, pending};
    if ((flags & TegraGM20BFenceSemanticFailed) && int32_t(seqno - failed) < 0)
        return {SemanticFenceStatus::Superseded, failed};
    return {};
}

// The newest job the record names failed, 0 for none (or a record the kernel is writing)
inline uint32_t newestSemanticFailure(const volatile TegraGM20BAbiFenceSemantic *record)
{
    const uint32_t before = __atomic_load_n(&record->epoch, __ATOMIC_ACQUIRE);
    const uint32_t failed = __atomic_load_n(&record->failed_first, __ATOMIC_RELAXED);
    const uint32_t flags = __atomic_load_n(&record->flags, __ATOMIC_RELAXED);
    std::atomic_thread_fence(std::memory_order_acquire);
    if ((before & 1) || __atomic_load_n(&record->epoch, __ATOMIC_RELAXED) != before ||
        !(flags & TegraGM20BFenceSemanticFailed))
        return 0;
    return failed;
}

inline SemanticFenceRead readSemanticFence(const volatile TegraGM20BAbiFenceSemantic *record, uint32_t seqno)
{
    return readSemanticFence(seqno, [record](unsigned field) {
        switch (field) {
        case 0:
            return __atomic_load_n(&record->epoch, __ATOMIC_ACQUIRE);
        case 1:
            return __atomic_load_n(&record->pending_first, __ATOMIC_RELAXED);
        case 2:
            return __atomic_load_n(&record->failed_first, __ATOMIC_RELAXED);
        default:
            return __atomic_load_n(&record->flags, __ATOMIC_RELAXED);
        }
    });
}

}  // namespace raw
