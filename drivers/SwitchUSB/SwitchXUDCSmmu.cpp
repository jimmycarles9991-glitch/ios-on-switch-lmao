// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h): the XUDC behind the MC SMMU (swgroup XUSB_DEV: clients xusb_devr and xusb_devw,
// TRANSLATION_ENABLE_2 bits 12 and 13, ASID register 0x28c; Linux drivers/memory/tegra/tegra210.c; L4T's device tree
// gives the XUDC iommus = <&smmu TEGRA_SWGROUP_XUSB_DEV>). All of RAM is mapped once at attach (UsbStaticMap.h): RAM below
// 4 GiB at its own address, so the rings, contexts, event ring, EP0's buffer and the bounce pool are reached as in
// bypass, and RAM above it 4 GiB lower. A request is translated by arithmetic; one the planner would bounce for its
// layout goes into the window (XudcIova.h). TegraSMMU programs the swgroup again after every SC7, before the XUDC powers
// on.
#include "SwitchXUDC.h"
#include "../TegraSMMU/TegraSMMU.h"

namespace {
constexpr uint32_t kTranslationBits = (1u << 12) | (1u << 13);
}  // namespace

void SwitchXUDC::smmuBegin()
{
    char why[160] = "";
    UsbRam ram = {};
    if (!UsbSmmuSpace::ram(&ram, why, sizeof(why)) ||
        !smmu.init(TEGRA_SMMU_ASID_XUSB_DEV, UsbStaticMap::device(ram), why, sizeof(why)) ||
        !smmu.attach(TEGRA_SMMU_XUSB_DEV_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, kTranslationBits, why, sizeof(why))) {
        IOLog("SwitchUSB: device DMA by physical address, high and broken buffers bounced (SMMU off: %s)\n", why);
        return;
    }
#if DEVELOPMENT || DEBUG
    IOLog("SwitchUSB: device DMA through the SMMU, RAM at fixed IOVAs, a %u-page window\n", smmu.windowPages());
#endif
}

// The runs the controller sees: in the window once remapped, else at their fixed IOVAs, or the request's own with the
// SMMU off. nullptr when a run is not in RAM.
const Run *SwitchXUDC::viewOf(const Request &r, uint32_t runCount, uint32_t *count)
{
    if (r.iovaPages) {
        *count = iovaRuns(scratchRuns, runCount, smmu.iova(r.iovaIndex), scratchIova);
        return scratchIova;
    }
    if (!smmu.attached()) {
        *count = runCount;
        return scratchRuns;
    }
    *count = fixedRuns(
        scratchRuns, runCount,
        [this](uint64_t pa, uint64_t length) {
            return smmu.fixedIova(pa, length);
        },
        scratchIova);
    return *count || !runCount ? scratchIova : nullptr;
}

// The request's pages into the window (XudcIova.h runPages), when it has room.
bool SwitchXUDC::remap(Request &r, uint32_t runCount)
{
    const uint32_t pages = runPages(scratchRuns, runCount, [](uint32_t, uint64_t) {});
    // All SwitchXUDC callers run on its single IOWorkLoop.
    const int64_t index = smmu.mapUnlocked(pages, kIovaAlignPages, [this, runCount](auto mapPage) {
        bool ok = true;
        runPages(scratchRuns, runCount, [&ok, &mapPage](uint32_t i, uint64_t pa) {
            ok = ok && mapPage(i, pa);
        });
        return ok;
    });
    if (index < 0)
        return false;
    r.iovaIndex = static_cast<uint32_t>(index);
    r.iovaPages = pages;
    return true;
}

// What the request held while the controller could reach its memory: the descriptor's preparation and its window
// pages. Only once the controller is past it (its event, or a cancel's dropQueued).
void SwitchXUDC::releaseMemory(Request &r)
{
    if (r.prepared)
        r.md->complete();
    r.prepared = false;
    if (r.iovaPages)
        smmu.unmapUnlocked(r.iovaIndex, r.iovaPages);
    r.iovaPages = 0;
}

// The bytes these runs would bounce, UINT32_MAX when they need more TRBs than a request may take: a window mapping is
// worth it from kRemapWorthBytes (XudcIova.h).
uint32_t SwitchXUDC::bounceBytes(const Run *runs, uint32_t count, uint32_t length, uint32_t mps, bool out)
{
    PlanStats why = {};
    const uint32_t n = plan(runs, count, length, mps, out, kReach, scratchPieces, kMaxPieces, &why);
    return n ? why.bounceBytes : UINT32_MAX;
}
