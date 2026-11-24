// SPDX-License-Identifier: MIT
// Debug builds: what a render state's creation did on its thread (TegraGM20BMetalStatesCost.mm), for its line and HW Test's
// gpu.pack.hit_cost. The thread counts the work where it does it, so a creation's costs are its thread's counts over it,
// whichever unit did the work: phases, pack lookups, program pieces imported and slots copied, pipelines built and
// variants placed here; kernel calls by the encode profile's Kernel span (encode_profile.h), locks by lock.h's LockCount.
#pragma once
#include "stats.h"

#include <cstdint>

#if SWITCH_METAL_DIAGNOSTICS
#include <mach/mach_time.h>

namespace gm20b {

struct CreationWork {
    // the creation's phases (TegraGM20BMetalStates.mm): its key, the pack's state placed, its packed variants, a
    // record the pack lacks
    uint64_t keyTicks = 0, placeTicks = 0, variantTicks = 0, recordTicks = 0;
    uint64_t lookups = 0, lookupTicks = 0;  // pack records looked up
    uint64_t copies = 0, copiedBytes = 0;   // program slots copied into the shader heap
    uint64_t pieces = 0, pieceTicks = 0;    // pieces of a file's pages imported and bound to run in place
    uint64_t builds = 0, buildTicks = 0;    // pipelines built from their records (Device::build)
    uint64_t variants = 0;                  // variant pipelines placed (Device::placeVariant)
    uint64_t textKeys = 0;                  // states' keys made as text (TegraGM20BMetalStatesKey.mm)
};

inline thread_local CreationWork tCreationWork;
// The process's Pack::load at its device's open (Device::open), in ticks
inline uint64_t gPackLoadTicks = 0;

// One creation's costs on its thread; ticks are mach_absolute_time's. probeTicks: what reading them took
struct CreationCost {
    uint64_t wallTicks = 0, probeTicks = 0;
    CreationWork work;
    uint64_t kernelCalls = 0;
    double kernelUs = 0;
    uint64_t locks = 0, lockWaits = 0, lockWaitTicks = 0;
    uint64_t faults = 0, pageins = 0;  // the task's, over the creation
};

// Adds the ticks from its making to its end to `ticks`
class CreationSpan {
public:
    explicit CreationSpan(uint64_t &ticks) : ticks_(ticks), began_(mach_absolute_time())
    {}

    ~CreationSpan()
    {
        ticks_ += mach_absolute_time() - began_;
    }

    CreationSpan(const CreationSpan &) = delete;
    CreationSpan &operator=(const CreationSpan &) = delete;

private:
    uint64_t &ticks_;
    uint64_t began_;
};

}  // namespace gm20b

#define SWITCH_METAL_CREATION_JOIN2(a, b) a##b
#define SWITCH_METAL_CREATION_JOIN(a, b)  SWITCH_METAL_CREATION_JOIN2(a, b)
#define SWITCH_METAL_CREATION_SPAN(field) \
    const gm20b::CreationSpan SWITCH_METAL_CREATION_JOIN(gm20bCreationSpan, __LINE__)(gm20b::tCreationWork.field)
#define SWITCH_METAL_CREATION_COUNT(field, n)    (gm20b::tCreationWork.field += (n))
#define SWITCH_METAL_CREATION_STAMP(name)        const uint64_t name = mach_absolute_time()
#define SWITCH_METAL_CREATION_SINCE(name, field) (gm20b::tCreationWork.field += mach_absolute_time() - (name))
#else
#define SWITCH_METAL_CREATION_SPAN(field)        static_cast<void>(0)
#define SWITCH_METAL_CREATION_COUNT(field, n)    static_cast<void>(0)
#define SWITCH_METAL_CREATION_STAMP(name)        static_cast<void>(0)
#define SWITCH_METAL_CREATION_SINCE(name, field) static_cast<void>(0)
#endif
