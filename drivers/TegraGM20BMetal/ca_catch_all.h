// SPDX-License-Identifier: MIT
// DEVELOPMENT: Core Animation's catch-all ("generic") state by its label, and the process's draws of one. QuartzCore
// names the state's spec after "com.apple.coreanimation." (Pipeline::Spec::name): a pipeline segment, then after '_'
// the fragment segment, whose X flags hold g for the catch-all in both builds' name tables (pack/ca/qc-<build>.json).
// Only the pipeline segment's pixel format codes have capitals besides the field letters, so the fragment segment's
// one X starts its flags.
#pragma once
#include "stats.h"
#if SWITCH_METAL_DIAGNOSTICS
#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace gm20b {

constexpr char kCaLabelPrefix[] = "com.apple.coreanimation.";

inline const char *caLabelSpec(const char *label)
{
    if (!label || strncmp(label, kCaLabelPrefix, sizeof kCaLabelPrefix - 1))
        return nullptr;
    const char *spec = label + sizeof kCaLabelPrefix - 1;
    return *spec ? spec : nullptr;
}

inline bool caCatchAll(const char *label)
{
    const char *spec = caLabelSpec(label);
    if (!spec)
        return false;
    const char *fragment = strchr(spec, '_');
    if (!fragment)
        return false;
    const char *end = strchr(++fragment, '_');
    const char *flags =
        static_cast<const char *>(memchr(fragment, 'X', end ? size_t(end - fragment) : strlen(fragment)));
    for (const char *f = flags ? flags + 1 : end; f && f != end && *f >= 'a' && *f <= 'z'; f++)
        if (*f == 'g')
            return true;
    return false;
}

// Recording threads add and the rates timer reads: a relaxed counter, the order of draws across threads unneeded
inline std::atomic<uint64_t> gCaCatchAllDraws{0};

// Sync render-state creations on fixed-priority threads (Core Animation's SCHED_FIFO render server) and their longest
// wall time in microseconds: relaxed atomics read by the rates timer, cross-thread ordering unneeded
inline std::atomic<uint64_t> gRenderSyncCreations{0};
inline std::atomic<uint64_t> gRenderSyncMaxUs{0};

// Distinct Core Animation labels answered in process (table, disk cache, or seed) logged this process
inline std::atomic<uint64_t> gInProcessCaLogged{0};

inline bool isFixedPriorityThread()
{
    int policy = SCHED_OTHER;
    sched_param param{};
    return pthread_getschedparam(pthread_self(), &policy, &param) == 0 && policy != SCHED_OTHER;
}

inline void recordRenderSyncCreation(uint64_t wallUs)
{
    gRenderSyncCreations.fetch_add(1, std::memory_order_relaxed);
    uint64_t prev = gRenderSyncMaxUs.load(std::memory_order_relaxed);
    while (wallUs > prev && !gRenderSyncMaxUs.compare_exchange_weak(prev, wallUs, std::memory_order_relaxed)) {
    }
}

inline void noteRenderSyncCreation(uint64_t wallUs)
{
    if (isFixedPriorityThread())
        recordRenderSyncCreation(wallUs);
}

}  // namespace gm20b
#endif
