// SPDX-License-Identifier: BSD-2-Clause-Patent
// One DMA master's identity view of RAM in the kernel (TegraSMMUIdentity.h): its directories, its lock, and its swgroup
// attached split by va[33:32]. Thread context throughout: a hold may allocate a page table.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOLocks.h>
#include "TegraSMMUIdentity.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

struct TegraSMMURamViewEnv {
    // Tables come from chunks of chunkTables pages: a 4 KiB table alone would take a whole VM page (16 KiB here).
    static constexpr uint32_t kChunkTables = 4;
    uint32_t chunkTables = kChunkTables;
    switch_dma_buffer chunk = {};  // the one tables are taken from, kept for good like them
    uint32_t chunkUsed = 0;

    TegraSMMUIdentityTable *makeTable();
    void clean(const void *va, size_t bytes);
    void sync();
    void flushAsid(uint32_t asid);
    void flush(uint32_t asid, const switch_smmu_table_flush *tables, uint32_t count);
};

class TegraSMMURamView {
public:
    // The directories for ASIDs low (PA below 4 GiB) and high. False when memory is short.
    // Pass needLock = false for single-owner views serialized externally by the caller.
    bool init(uint32_t asidLow, uint32_t asidHigh, bool needLock = true);
    // The swgroup's fields 0 and 1 to this view, field 2 to (asid2, ptb2) or, with asid2 0, the fault ASID, field 3 to
    // the fault ASID, for each translation register the swgroup's clients are in. 0, or -1 with one line.
    int attach(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, bool alwaysTranslated,
               uint32_t asid2, uint32_t ptb2, char *line, size_t size);
    // Every page table of RAM below `limit` made and installed now (one 4 MiB buffer and 2 KiB of holder counts per 4 MiB of RAM: 6
    // MiB), so holds never allocate.
    bool reserveTables(uint64_t limit = TegraSMMUIdentityCore<TegraSMMURamViewEnv>::kRamEnd);
    // A 32-bit client's swgroup (one ASID field) to the view's directory for RAM below 4 GiB. 0, or -1 with one line.
    int attachNarrow(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, char *line, size_t size);
    // The device may use [pa, pa + bytes) once a commit follows; false (nothing held) as TegraSMMUIdentityCore::hold.
    bool hold(uint64_t pa, uint64_t bytes);
    // A hold ends; the page may go back to the VM once a commit follows. A release with no hold panics.
    void release(uint64_t pa, uint64_t bytes);
    void commit();
    // hold of each page of `pages` (4 KiB physical addresses) and a commit; all or nothing.
    bool holdPages(const uint64_t *pages, uint32_t count);
    // release of each page of `pages` and a commit.
    void releasePages(const uint64_t *pages, uint32_t count);

    // body(core) under the view's lock: a batch of TegraSMMUIdentityCore holds and releases and their commit take the
    // lock once.
    template <class Body> void locked(Body &&body)
    {
        if (lock_)
            IOLockLock(lock_);
        body(core_);
        if (lock_)
            IOLockUnlock(lock_);
    }

    // body(core) without locking: only for single-owner views serialized by their caller.
    template <class Body> void unlocked(Body &&body)
    {
        body(core_);
    }

    bool ready() const
    {
        return ready_;
    }

private:
    bool ready_ = false;
    IOLock *lock_ = nullptr;
    switch_dma_buffer low_ = {}, high_ = {};
    TegraSMMURamViewEnv env_;
    TegraSMMUIdentityCore<TegraSMMURamViewEnv> core_;
};
