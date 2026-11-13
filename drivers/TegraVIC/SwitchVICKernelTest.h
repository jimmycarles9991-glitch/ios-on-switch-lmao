// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraVIC's probes, debug kernels only: IOSurfaceAcceleratorClient selector 6 (Apple's kernel-test selector, which a
// release kernel refuses with kIOReturnNotPermitted), struct in {magic, 0}, struct out the probe's reply. The magic
// names the probe (each probe's header has its reply); a probe runs VIC jobs on two VIC-mapped bounce buffers, which
// the CPU fills and reads only under the backing's CPU leases. A job that never completes panics in the retire
// thread, and tegra_vic_describe names the probe case that was running, so that boot still says which one it was.
#pragma once
#include "TegraVIC.h"

#if TEGRA_VIC_STATS
#include "SwitchVICBouncePool.h"

struct SwitchVICProbeBuffers {
    SwitchVICBounceSlot *slot;
    uint8_t *bytes[2];    // source, destination
    uint64_t address[2];  // the same in VIC's address space
    uint64_t size;
};

int switch_vic_probe_buffers(uint64_t bytes, SwitchVICProbeBuffers *out);
void switch_vic_probe_buffers_release(SwitchVICProbeBuffers *buffers);

int switch_vic_probe_cpu_begin(const SwitchVICProbeBuffers &buffers, uint32_t which, uint32_t access);
int switch_vic_probe_cpu_end(const SwitchVICProbeBuffers &buffers, uint32_t which, uint32_t access);

// `touch` gets buffer `which`'s bytes between the lease's acquire and release.
template <class Touch>
int switch_vic_probe_cpu(const SwitchVICProbeBuffers &buffers, uint32_t which, uint32_t access, Touch &&touch)
{
    if (const int error = switch_vic_probe_cpu_begin(buffers, which, access))
        return error;
    touch(buffers.bytes[which]);
    return switch_vic_probe_cpu_end(buffers, which, access);
}

// One job reading buffer 0 and writing buffer 1, run to completion. `probe` and `index` name it while it runs.
int switch_vic_probe_submit(const SwitchVICProbeBuffers &buffers, const TegraVICJob &job, uint32_t probe,
                            uint32_t index);

// The probe case a job is running for ("probe VBLK case 3"), or an empty string.
const char *switch_vic_probe_running();

int switch_vic_pitch_probe_run(void *reply);
int switch_vic_layout_probe_run(void *reply);
int switch_vic_fraction_probe_run(void *reply);
int switch_vic_packed_probe_run(void *reply);
int switch_vic_transpose_probe_run(void *reply);
int switch_vic_large_probe_run(void *reply);
int switch_vic_compose_probe_run(void *reply);
int switch_vic_composite_arm_run(void *reply);
int switch_vic_composite_report_run(void *reply);
#endif
