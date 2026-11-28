// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's command DMA through the SMMU (swgroups HC and HC1, one ASID field each; SwitchHost1xChannels.h
// switch_host1x_channels_confine). host1x fetches only the push buffers and the arenas their GATHERs point into, all
// the driver's own and below 4 GiB, so they are held for good and every other address faults at the MC. host1xdmar
// may tag a fetch with the in-band SWID hc1 instead of hc (TRM 18.5.3), so both swgroups go to the one view; a
// confinement that cannot be whole is undone.
#include <IOKit/IOLib.h>
#include "SwitchHost1xChannels.h"
#include "TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
TegraSMMURamView gView;
bool gAttached;
// host1xdmar and host1xr (TRANSLATION_ENABLE_0 bits 22/23), host1xw (TRANSLATION_ENABLE_1 bit 22): T210 TRM
// 18.11.1.66-67.
constexpr uint32_t kHostReads = 1u << 22 | 1u << 23, kHostWrites = 1u << 22, kChannelFetch = 1u << 22;

struct Swgroup {
    uint32_t asidReg, translationReg, bits;
};

constexpr Swgroup kSwgroups[] = {
    {TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, kHostReads},
    {TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, kHostWrites},
    {TEGRA_SMMU_HC1_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, kChannelFetch},
};
}  // namespace

bool switch_host1x_channels_confine(char *line, size_t size)
{
    const switch_dma_buffer *buffers[2 * TEGRA_HOST1X_CHANNELS] = {};
    const uint32_t count = switch_host1x_channels_buffers(buffers, 2 * TEGRA_HOST1X_CHANNELS);
    if (!gView.init(TEGRA_SMMU_ASID_HOST1X, TEGRA_SMMU_ASID_HOST1X)) {
        snprintf(line, size, "no memory for its directories");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (buffers[i]->pa + buffers[i]->size > (1ULL << 32) || !gView.hold(buffers[i]->pa, buffers[i]->size)) {
            snprintf(line, size, "no page table for buffer %#llx", static_cast<unsigned long long>(buffers[i]->pa));
            return false;
        }
    }
    gView.commit();
    for (uint32_t i = 0; i < sizeof(kSwgroups) / sizeof(kSwgroups[0]); ++i) {
        const Swgroup &g = kSwgroups[i];
        if (!gView.attachNarrow(g.asidReg, g.translationReg, g.bits, line, size))
            continue;
        for (uint32_t j = 0; j < i; ++j)
            tegra_smmu_detach(TEGRA_SMMU_ASID_HOST1X, kSwgroups[j].asidReg);
        return false;
    }
    gAttached = true;
    return true;
}

bool switch_host1x_channels_confine_channel(const switch_dma_buffer *push, const switch_dma_buffer *arena)
{
    if (!gAttached)
        return true;
    if (!push || !arena || push->pa + push->size > (1ULL << 32) || arena->pa + arena->size > (1ULL << 32))
        return false;
    if (!gView.hold(push->pa, push->size))
        return false;
    if (!gView.hold(arena->pa, arena->size)) {
        gView.release(push->pa, push->size);
        gView.commit();
        return false;
    }
    gView.commit();
    return true;
}
