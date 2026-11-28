// SPDX-License-Identifier: BSD-2-Clause-Patent
// The MC fault probe (SwitchVICMcFaultProbe.h), debug kernels only.
#include "SwitchVICMcFaultProbe.h"
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>
#include "../TegraSMMU/TegraSMMU.h"

namespace {
using namespace switch_vic_mc_fault_probe;

constexpr uint32_t kDeliveryMs = 100;  // after the copy: the interrupt takes microseconds
static_assert(kFaultIova + kFaultBytes <= kTegraVICWindowBase, "VIC's directory is empty below its windows");
}  // namespace

int switch_vic_mc_fault_probe_run(void *out)
{
    auto *reply = static_cast<SwitchVICMcFaultProbeReply *>(out);
    reply->magic = SWITCH_VIC_MC_FAULT_PROBE_MAGIC;
    reply->iova = kFaultIova;
    SwitchVICProbeBuffers buffers;
    const int error = switch_vic_probe_buffers(kBufferBytes, &buffers);
    TegraVICJob job;
    if (!error && build(buffers.address[1], tegra_vic_config_address(), tegra_vic_filter_address(), &job)) {
        reply->status = -95;
    } else if (!error) {
        switch_mc_errors_expect(kVicRead, kFaultIova, kFaultIova + kFaultBytes);
        reply->status = switch_vic_probe_submit(buffers, job, SWITCH_VIC_MC_FAULT_PROBE_MAGIC, 0);
        switch_mc_expected got;
        switch_mc_errors_expect_end(kDeliveryMs, &got);
        reply->attached = got.attached;
        reply->taken = got.taken;
        reply->intstatus = got.intstatus;
        reply->errStatus = got.status;
        reply->address = got.address;
    }
    switch_vic_probe_buffers_release(&buffers);
    IOLog("TegraVIC: MC fault probe: copy from %#x status %d, %u SMMU faults delivered by the MC error interrupt "
          "(attached %u), the first INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x\n",
          reply->iova, reply->status, reply->taken, reply->attached, reply->intstatus, reply->errStatus,
          reply->address);
    return error;
}
#endif
