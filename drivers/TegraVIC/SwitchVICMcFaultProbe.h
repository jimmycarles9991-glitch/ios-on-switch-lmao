// SPDX-License-Identifier: BSD-2-Clause-Patent
// The MC fault probe, debug kernels only (selector 6, SwitchVICKernelTest.h): whether the memory controller's error
// interrupt delivers. A VIC copy reads its source at kFaultIova, which VIC's page directory leaves empty (below its
// windows), so each of its reads faults at the SMMU: INVALID_SMMU_PAGE by csr_vicsrd. The MC error filter takes those
// as expected instead of panicking (TegraSMMU.h switch_mc_errors_expect), and once the copy is over the probe waits for
// the interrupt to have delivered them; a fault still latched then panics as a lost interrupt. Plain logic, no IOKit:
// SwitchVICMcFaultProbe.cpp runs it, test/switch_vic_mc_fault_probe_test.cpp builds its copy on test/vic_model.h.
#pragma once
#include <stdint.h>
#include "TegraVICConfig.h"

#define SWITCH_VIC_MC_FAULT_PROBE_MAGIC 0x564d4346u  // 'VMCF'

typedef struct {
    uint32_t magic;
    int32_t status;                          // 0, -95 refused by tegra_vic_build, else the copy's error
    uint32_t iova;                           // where the copy read
    uint32_t attached;                       // 0: the MC error interrupt was never attached
    uint32_t taken;                          // the faults the interrupt delivered
    uint32_t intstatus, errStatus, address;  // the first of them
} SwitchVICMcFaultProbeReply;

namespace switch_vic_mc_fault_probe {

constexpr uint32_t kFaultIova = 0x08000000;
constexpr uint32_t kFaultBytes = 0x10000;  // what the MC filter expects: the copy's reads and its cache's row fetches
constexpr uint32_t kPixels = 64, kRows = 4, kPitch = kPixels * 4, kBufferBytes = 4096;
constexpr uint32_t kVicRead = 0x6c;  // csr_vicsrd (T210 TRM 18.11.1 client table)

inline TegraVICImage bgra(uint64_t address)
{
    TegraVICImage image = {};
    image.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    image.width = kPixels;
    image.height = kRows;
    image.pitch[0] = kPitch;
    image.rows[0] = kRows;
    image.base[0] = address;
    image.span[0] = kBufferBytes;
    return image;
}

// The copy from kFaultIova to `destination`, 1:1; null, or why VIC refuses it.
inline const char *build(uint64_t destination, uint64_t config, uint64_t filter, TegraVICJob *job)
{
    const TegraVICImage in = bgra(kFaultIova), out = bgra(destination);
    TegraVICOp op = {};
    op.srcW = uint64_t(kPixels) << 16;
    op.srcH = uint64_t(kRows) << 16;
    op.dstW = kPixels;
    op.dstH = kRows;
    op.nearest = 1;
    return tegra_vic_build(&in, &out, &op, &kTegraVICTraits, config, filter, job);
}

}  // namespace switch_vic_mc_fault_probe
