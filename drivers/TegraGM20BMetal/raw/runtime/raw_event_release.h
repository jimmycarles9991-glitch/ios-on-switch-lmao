// SPDX-License-Identifier: MIT
// A kernel event's signal released mid-job into its word (drivers/TegraGM20B/TegraGM20BJobABI.h method 40): the 4-byte host
// release after the wait for idle (RELEASE_WFI), then the nonstall interrupt the kernel completes the signal on. The word
// needs no flush: the fence pool is mapped volatile (gpu_chan.c tgm_fences_map). What the work before it wrote does, for a
// CPU reader, when that is host-visible memory: the GPU's L2 holds it under l2_on() and the CPU does not snoop the L2, and
// only the host's MEM_OP_D L2_FLUSH_DIRTY (clb06f.h) writes it back, after a wait for idle; the engines' own releases (3D and
// compute SET_REPORT_SEMAPHORE_D, the copy engine's LAUNCH_DMA semaphore) carry no L2 writeback. So a release `publishes`
// (the flush first, as the kernel's external job end makes it, gpu_exec.c tgm_chan_fence_job) only where host-visible
// memory was written since the last one that did, by the job end's own rule (CommandBuffer::use), and always on a kernel
// without addressed jobs, whose every job end publishes (gpu_exec.c tgm_chan_exec: external). Other readers of the
// word (another channel's acquire, the kernel) read it, and what the GPU wrote, through the same L2.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"

namespace raw {

constexpr uint32_t kEventReleaseWords = 16;

// Host-visible memory written since the last publishing release: whether the next release publishes
class EventPublication {
public:
    void wrote(bool hostVisible)
    {
        pending_ |= hostVisible;
    }

    bool take()
    {
        const bool publish = pending_;
        pending_ = false;
        return publish;
    }

private:
    bool pending_ = false;
};

inline void emitEventRelease(Push &p, uint64_t va, uint32_t payload, bool publish)
{
    if (publish) {
        p.inc(NVB06F_WFI, {0});
        p.inc(NVB06F_FB_FLUSH, {0});
        p.inc(NVB06F_MEM_OP_C, {0, NVDEF(NVB06F, MEM_OP_D, OPERATION, L2_FLUSH_DIRTY)});
        p.inc(NVB06F_FB_FLUSH, {0});
    }
    p.inc(NVB06F_SEMAPHOREA,
          {uint32_t(va >> 32) & 0xff, uint32_t(va), payload,
           NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) | NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN) |
               NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE)});
    p.inc(NVB06F_NON_STALL_INTERRUPT, {0});
}

// The payload a signal of `value` releases into a word of `base` whose last payload was `floor`; 0 where its job's end
// signals it instead (outside the word's window, or no later than a payload released before)
inline uint32_t eventReleasePayload(uint64_t value, uint64_t base, uint32_t floor)
{
    const uint64_t payload = value - base;
    return payload > floor && payload < (uint64_t(1) << 31) ? uint32_t(payload) : 0;
}

}  // namespace raw
