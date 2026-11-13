// SPDX-License-Identifier: MIT
// Debug builds: the allocations an encode makes, counted through libmalloc's malloc_logger hook, which every zone
// calls for every allocation and free while it is set. It is set only while some thread encodes and never over a hook
// someone else set (MallocStackLogging), in which case the encode is not counted. The hook finds the encoding
// thread's profile through a pthread key: a C++ thread_local could allocate its storage on a thread's first read, from
// inside malloc.
#pragma once
#include "raw_encode_profile.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <pthread.h>

typedef void(malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                              uint32_t num_hot_frames_to_skip);
extern "C" malloc_logger_t *malloc_logger;

namespace raw {

inline pthread_key_t encodeMallocKey()
{
    static const pthread_key_t key = [] {
        pthread_key_t made = 0;
        pthread_key_create(&made, nullptr);
        return made;
    }();
    return key;
}

inline void countEncodeMallocs(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    auto *profile = static_cast<EncodeProfile *>(pthread_getspecific(encodeMallocKey()));
    if (!profile)
        return;
    constexpr uint32_t kAllocate = 2, kDeallocate = 4;
    if (type & kAllocate)
        profile->counts[size_t(EncodeCount::Mallocs)]++;
    if (type & kDeallocate)
        profile->counts[size_t(EncodeCount::Frees)]++;
}

class EncodeMallocs {
public:
    explicit EncodeMallocs(EncodeProfile &profile) : profile_(profile)
    {
        const pthread_key_t key = encodeMallocKey();
        if (active().fetch_add(1, std::memory_order_acq_rel) == 0 && !malloc_logger)
            malloc_logger = countEncodeMallocs;
        profile_.mallocsCounted = malloc_logger == countEncodeMallocs;
        pthread_setspecific(key, &profile_);
    }

    ~EncodeMallocs()
    {
        pthread_setspecific(encodeMallocKey(), nullptr);
        if (active().fetch_sub(1, std::memory_order_acq_rel) == 1 && malloc_logger == countEncodeMallocs)
            malloc_logger = nullptr;
    }

    EncodeMallocs(const EncodeMallocs &) = delete;
    EncodeMallocs &operator=(const EncodeMallocs &) = delete;

private:
    static std::atomic<int> &active()
    {
        static std::atomic<int> encodes{0};
        return encodes;
    }

    EncodeProfile &profile_;
};

}  // namespace raw
#endif
