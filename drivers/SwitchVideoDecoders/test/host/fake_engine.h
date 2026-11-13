// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDEC's job queue for the decoders' host tests, with NVDEC as slow as it may be: a job ends only when the next
// Submit needs its place in the full queue (TegraNVDECABI.h, Queue room), or when the decoder waits for it. Counted:
// the decoder's waits, and every range a Submit hands over as written by the CPU (its clean ranges) in a buffer a
// queued job still uses, which the kernel would sleep on before queueing the job.
#ifndef FAKE_ENGINE_H
#define FAKE_ENGINE_H
#include <stdint.h>
#include "../../../TegraNVDEC/TegraNVDECABI.h"

enum {
    FAKE_ENGINE_DEPTH = 8,  // SwitchNVDECQueue::kSlots
    FAKE_ENGINE_HANDLES = 65,
};

typedef struct {
    uint32_t submitted, completed;
    uint32_t user[FAKE_ENGINE_HANDLES];  // the newest job naming the buffer
    unsigned waits, busyWrites;
} FakeEngine;

static inline bool fake_engine_pending(const FakeEngine *e, uint32_t fence)
{
    return (int32_t)(fence - e->completed) > 0;
}

static inline void fake_engine_allocated(FakeEngine *e, uint32_t handle)
{
    if (handle < FAKE_ENGINE_HANDLES)
        e->user[handle] = 0;
}

static inline uint32_t fake_engine_submit(FakeEngine *e, const TegraNVDECSubmit *s)
{
    for (uint32_t i = 0; i < s->clean_count; i++) {
        const uint32_t h = s->clean[i].handle;
        if (h < FAKE_ENGINE_HANDLES && e->user[h] && fake_engine_pending(e, e->user[h]))
            e->busyWrites++;
    }
    if (e->submitted - e->completed >= FAKE_ENGINE_DEPTH)
        e->completed = e->submitted - FAKE_ENGINE_DEPTH + 1;
    const uint32_t fence = ++e->submitted;
    for (uint32_t i = 0; i < s->entry_count; i++)
        if (s->entries[i].kind == TEGRA_NVDEC_ENTRY_RELOC && s->entries[i].value < FAKE_ENGINE_HANDLES)
            e->user[s->entries[i].value] = fence;
    return fence;
}

static inline void fake_engine_wait(FakeEngine *e, uint32_t fence)
{
    e->waits++;
    if (fake_engine_pending(e, fence))
        e->completed = fence;
}

#endif
