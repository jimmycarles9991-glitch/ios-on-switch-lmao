// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraSMMURamView.h.
#include <IOKit/IOLib.h>
#include "TegraSMMU.h"
#include "TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

TegraSMMUIdentityTable *TegraSMMURamViewEnv::makeTable()
{
    if (chunk.va && chunkUsed == chunk.size / TegraSMMUDomain::kPage)
        chunk = {};
    if (!chunk.va) {
        if (!switch_dma_alloc(&chunk, static_cast<size_t>(chunkTables) * TegraSMMUDomain::kPage)) {
            chunk = {};
            return nullptr;
        }
        chunkUsed = 0;
    }
    auto *table = IONew(TegraSMMUIdentityTable, 1);
    auto *users = IONew(uint16_t, TegraSMMUDomain::kEntries);
    if (!table || !users) {
        if (table)
            IODelete(table, TegraSMMUIdentityTable, 1);
        if (users)
            IODelete(users, uint16_t, TegraSMMUDomain::kEntries);
        return nullptr;
    }
    bzero(users, TegraSMMUDomain::kEntries * sizeof(uint16_t));
    const uint64_t at = static_cast<uint64_t>(chunkUsed++) * TegraSMMUDomain::kPage;
    *table = {reinterpret_cast<uint32_t *>(chunk.va + at), chunk.pa + at, 0, users, 0, 0, nullptr, 0, 0, 0, {}};
    return table;  // kept for good with its chunk: a view's tables outlive every hold
}

void TegraSMMURamViewEnv::clean(const void *va, size_t bytes)
{
    switch_dcache_clean_lines(va, bytes);
}

void TegraSMMURamViewEnv::sync()
{
    switch_dcache_sync();
}

void TegraSMMURamViewEnv::flushAsid(uint32_t asid)
{
    tegra_smmu_flush(asid);
}

void TegraSMMURamViewEnv::flush(uint32_t asid, const switch_smmu_table_flush *tables, uint32_t count)
{
    switch_smmu_flush_tables(asid, tables, count);
}

bool TegraSMMURamView::init(uint32_t asidLow, uint32_t asidHigh, bool needLock)
{
    if (ready_)
        return true;
    IOLock *lock = needLock ? IOLockAlloc() : nullptr;
    if ((needLock && !lock) || !switch_dma_alloc(&low_, TegraSMMUDomain::kPage) ||
        !switch_dma_alloc(&high_, TegraSMMUDomain::kPage) ||
        !core_.init(&env_, asidLow, reinterpret_cast<uint32_t *>(low_.va), low_.pa, asidHigh,
                    reinterpret_cast<uint32_t *>(high_.va), high_.pa)) {
        switch_dma_free(&low_);
        switch_dma_free(&high_);
        if (lock)
            IOLockFree(lock);
        return false;
    }
    lock_ = lock;
    ready_ = true;
    return true;
}

int TegraSMMURamView::attach(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, bool alwaysTranslated,
                             uint32_t asid2, uint32_t ptb2, char *line, size_t size)
{
    const uint32_t fault = tegra_smmu_fault_ptb();
    if (!ready_ || !fault) {
        snprintf(line, size, "no memory for the view's directories or the fault directory");
        return -1;
    }
    const uint32_t asids[4] = {core_.halves[0].asid, core_.halves[1].asid, asid2 ? asid2 : TEGRA_SMMU_ASID_FAULT,
                               TEGRA_SMMU_ASID_FAULT};
    const uint32_t ptbs[4] = {core_.ptb(0), core_.ptb(1), asid2 ? ptb2 : fault, fault};
    return tegra_smmu_attach_split(asids, ptbs, asidReg, translationReg, translationBits, alwaysTranslated, line, size);
}

int TegraSMMURamView::attachNarrow(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, char *line,
                                   size_t size)
{
    if (!ready_) {
        snprintf(line, size, "no memory for the view's directories");
        return -1;
    }
    return tegra_smmu_attach(core_.halves[0].asid, core_.ptb(0), asidReg, translationReg, translationBits, line, size);
}

bool TegraSMMURamView::reserveTables(uint64_t limit)
{
    using Core = TegraSMMUIdentityCore<TegraSMMURamViewEnv>;
    bool made = false;
    locked([&](auto &core) {
        env_.chunkTables =
            static_cast<uint32_t>(((limit < Core::kRamEnd ? limit : Core::kRamEnd) - Core::kRamBase) / Core::kSpan);
        made = core.makeTables(limit);
        env_.chunkTables = TegraSMMURamViewEnv::kChunkTables;
        core.commit();
    });
    return made;
}

bool TegraSMMURamView::hold(uint64_t pa, uint64_t bytes)
{
    bool held = false;
    locked([&](auto &core) {
        held = core.hold(pa, bytes);
    });
    return held;
}

void TegraSMMURamView::release(uint64_t pa, uint64_t bytes)
{
    bool released = false;
    locked([&](auto &core) {
        released = core.release(pa, bytes);
    });
    if (!released)
        panic("TegraSMMU: ASID %u: release of %#llx + %#llx, a page of which has no holder", core_.halves[0].asid,
              static_cast<unsigned long long>(pa), static_cast<unsigned long long>(bytes));
}

void TegraSMMURamView::commit()
{
    locked([](auto &core) {
        core.commit();
    });
}

bool TegraSMMURamView::holdPages(const uint64_t *pages, uint32_t count)
{
    uint32_t held = 0;
    locked([&](auto &core) {
        while (held < count && core.hold(pages[held], TegraSMMUDomain::kPage))
            ++held;
        if (held < count) {
            for (uint32_t i = 0; i < held; ++i)
                (void)core.release(pages[i], TegraSMMUDomain::kPage);
        }
        core.commit();
    });
    return held == count;
}

void TegraSMMURamView::releasePages(const uint64_t *pages, uint32_t count)
{
    bool released = true;
    locked([&](auto &core) {
        for (uint32_t i = 0; i < count; ++i)
            released &= core.release(pages[i], TegraSMMUDomain::kPage);
        core.commit();
    });
    if (!released)
        panic("TegraSMMU: ASID %u: release of %u pages from %#llx, one of which has no holder", core_.halves[0].asid,
              count, static_cast<unsigned long long>(count ? pages[0] : 0));
}
