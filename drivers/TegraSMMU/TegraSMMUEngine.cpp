// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraSMMUEngine.h.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "TegraSMMU.h"
#include "TegraSMMUEngine.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"

namespace {
constexpr uint64_t kWindowLimit = 2ULL << 30;

// The table entries written since the last call, to memory: the SMMU does not snoop the CPU's caches.
void cleanDirty(TegraSMMUDomain &domain)
{
    uintptr_t low = 0;
    uintptr_t high = 0;
    if (domain.takeDirty(&low, &high))
        switch_dcache_clean(reinterpret_cast<const void *>(low), high - low);
}
}  // namespace

bool TegraSMMUEngine::init(uint32_t engineAsid, uint64_t windowBase, uint64_t windowBytes, uint32_t identityTables)
{
    if (!windowBase || (windowBase | windowBytes) % TegraSMMUDomain::kSpan || windowBase + windowBytes > kWindowLimit)
        return false;
    const uint32_t count = 1 + static_cast<uint32_t>(windowBytes / TegraSMMUDomain::kSpan) + identityTables;
    bitmapWords = TegraSMMUSpace::bitmapWords(windowBytes);
    bitmap = IONew(uint32_t, bitmapWords);
    lock = IOLockAlloc();
    if (!bitmap || !lock || !switch_dma_alloc(&tables, static_cast<size_t>(count) * SWITCH_HW_PAGE_SIZE) ||
        !domain.init(tables.va, tables.pa, count) || !domain.reserve(windowBase, windowBytes) ||
        !space.init(windowBase, windowBytes, bitmap, bitmapWords)) {
        release();
        return false;
    }
    asid = engineAsid;
    switch_dcache_clean(tables.va, tables.size);
    uintptr_t low = 0;
    uintptr_t high = 0;
    domain.takeDirty(&low, &high);
    return true;
}

void TegraSMMUEngine::release()
{
    switch_dma_free(&tables);
    if (bitmap)
        IODelete(bitmap, uint32_t, bitmapWords);
    if (lock)
        IOLockFree(lock);
    *this = TegraSMMUEngine{};
}

bool TegraSMMUEngine::identity(uint64_t pa, uint64_t bytes, bool writable)
{
    const bool ok = pa >= space.base + space.bytesTotal() && domain.identity(pa, bytes, writable);
    cleanDirty(domain);
    return ok;
}

int TegraSMMUEngine::attach(const TegraSMMUSwgroup *engineGroups, uint32_t count, char *line, size_t size)
{
    groups = engineGroups;
    groupCount = count;
    for (uint32_t i = 0; i < count; i++) {
        if (tegra_smmu_attach(asid, domain.ptb(), groups[i].asidReg, groups[i].translationReg,
                              groups[i].translationBits, line, size))
            return -1;
    }
    return 0;
}

// NOLINTNEXTLINE(readability-make-member-function-const): detaching changes the engine's hardware state
void TegraSMMUEngine::detach()
{
    for (uint32_t i = 0; i < groupCount; i++)
        tegra_smmu_detach(asid, groups[i].asidReg);
}

uint64_t TegraSMMUEngine::map(const uint64_t *pages, uint32_t count, bool writable)
{
    for (uint32_t i = 0; i < count; i++) {
        if (pages[i] >= kPageLimit || (pages[i] & SWITCH_HW_PAGE_MASK))
            return 0;
    }
    IOLockLock(lock);
    const uint64_t iova = count ? space.alloc(static_cast<uint64_t>(count) * SWITCH_HW_PAGE_SIZE) : 0;
    if (iova) {
        domain.map(iova, pages, count, writable);  // the window's tables are all installed: it cannot fail
        cleanDirty(domain);
        tegra_smmu_flush(asid);
    }
    IOLockUnlock(lock);
    return iova;
}

void TegraSMMUEngine::unmap(uint64_t iova, uint32_t count)
{
    if (!iova || !count)
        return;
    IOLockLock(lock);
    domain.unmap(iova, count);
    cleanDirty(domain);
    tegra_smmu_flush(asid);
    space.release(iova, static_cast<uint64_t>(count) * SWITCH_HW_PAGE_SIZE);
    IOLockUnlock(lock);
}

void TegraSMMUEngine::usage(uint64_t *total, uint64_t *free) const
{
    IOLockLock(lock);
    *total = space.bytesTotal();
    *free = space.bytesFree();
    IOLockUnlock(lock);
}
