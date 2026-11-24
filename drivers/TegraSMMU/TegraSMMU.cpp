// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 MC SMMU registers under one lock (TegraSMMU.h). Short register sequences only: no allocation, no waits.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include "TegraSMMU.h"
#include "TegraSMMUAttachments.h"
#include "SwitchSmmuRegisters.h"
#include "TegraMCNames.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr vm_offset_t kMcPA = 0x70019000;

enum : uint32_t {
    PTB_ASID = 0x1c,
    PTB_DATA = 0x20,
    TLB_FLUSH = 0x30,
    PTC_FLUSH = 0x34,
    ASID_ENABLE = 1u << 31,
    TLB_FLUSH_ASID_MATCH = 1u << 31,
};

volatile uint8_t *gMc;
IOSimpleLock *gLock;
tegra_smmu_attachments gAttached;  // under gLock

inline uint32_t r32(uint32_t o)
{
    return switch_mmio_read32(gMc, o);
}

inline void w32(uint32_t o, uint32_t v)
{
    switch_mmio_write32(gMc, o, v);
}

// The registers and the lock, made by the first caller (thread context).
bool ready()
{
    if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE)) {
        IOSimpleLock *fresh = IOSimpleLockAlloc();
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        IOSimpleLock *expected = nullptr;
        if (!fresh)
            return false;
        if (!__atomic_compare_exchange_n(&gLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOSimpleLockFree(fresh);
    }
    if (!__atomic_load_n(&gMc, __ATOMIC_ACQUIRE)) {
        // NOLINTNEXTLINE(misc-const-correctness): gMc holds a pointer to non-const registers
        volatile uint8_t *mc = switch_mmio_map(kMcPA, 0x1000);
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        volatile uint8_t *expected = nullptr;
        if (!mc)
            return false;
        __atomic_compare_exchange_n(&gMc, &expected, mc, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return true;
}

struct RegisterAccess {
    static uint32_t read(uint32_t offset)
    {
        return r32(offset);
    }

    static void write(uint32_t offset, uint32_t value)
    {
        w32(offset, value);
    }
};

void flushLocked(uint32_t asid)
{
    RegisterAccess io;
    switch_smmu::flush(io, asid);
}

int programLocked(const tegra_smmu_attachment &a, const tegra_smmu_attachments &attachments, char *line, size_t size)
{
    RegisterAccess io;
    return switch_smmu::program(io, a, line, size, &attachments);
}
}  // namespace

namespace {
int attach(const tegra_smmu_attachment &attachment, char *line, size_t size)
{
    if (!ready()) {
        snprintf(line, size, "cannot map the memory controller");
        return -1;
    }
    IOSimpleLockLock(gLock);
    int ret = -1;
    const uint32_t was = r32(attachment.asidReg);
    if ((was & ASID_ENABLE) && (was & 0x7f) != attachment.asid) {
        snprintf(line, size, "the swgroup is already translated by ASID %u (MC %#x = %#x)", was & 0x7f,
                 attachment.asidReg, was);
    } else {
        bool conflict = false;
        for (uint32_t i = 0; i < gAttached.count; ++i) {
            const auto &owned = gAttached.entry[i];
            conflict |=
                (owned.asid == attachment.asid && owned.ptb != attachment.ptb) ||
                (owned.asidReg == attachment.asidReg &&
                 (owned.asid != attachment.asid || owned.ptb != attachment.ptb || owned.wide != attachment.wide));
        }
        tegra_smmu_attachments pending = gAttached;
        if (conflict)
            snprintf(line, size, "ASID %u or swgroup %#x already owns another directory", attachment.asid,
                     attachment.asidReg);
        else if (!tegra_smmu_attachments_note(&pending, &attachment))
            snprintf(line, size, "no retained slot for ASID %u swgroup %#x", attachment.asid, attachment.asidReg);
        else {
            ret = programLocked(attachment, pending, line, size);
            if (!ret)
                gAttached = pending;
        }
    }
    IOSimpleLockUnlock(gLock);
    return ret;
}
}  // namespace

extern "C" int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                                 uint32_t translationBits, char *line, size_t size)
{
    return attach({asid, ptb, asidReg, translationReg, translationBits, false}, line, size);
}

extern "C" int tegra_smmu_attach_wide(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                                      uint32_t translationBits, char *line, size_t size)
{
    return attach({asid, ptb, asidReg, translationReg, translationBits, true}, line, size);
}

extern "C" int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t asidReg, char *line, size_t size)
{
    if (!ready()) {
        snprintf(line, size, "cannot map the memory controller");
        return -1;
    }
    IOSimpleLockLock(gLock);
    RegisterAccess io;
    const int result = switch_smmu::restore(io, gAttached, asid, ptb, asidReg, line, size);
    IOSimpleLockUnlock(gLock);
    return result;
}

extern "C" void tegra_smmu_detach(uint32_t asid, uint32_t asidReg)
{
    if (!ready())
        return;
    IOSimpleLockLock(gLock);
    tegra_smmu_attachments_forget(&gAttached, asidReg);
    w32(asidReg, 0);
    (void)r32(asidReg);
    w32(PTB_ASID, asid);
    w32(PTB_DATA, 0);
    flushLocked(asid);
    IOSimpleLockUnlock(gLock);
    switch_mc_errors_detached(asidReg);
}

extern "C" void tegra_smmu_flush(uint32_t asid)
{
    if (!ready())
        return;
    IOSimpleLockLock(gLock);
    flushLocked(asid);
    IOSimpleLockUnlock(gLock);
}

extern "C" void switch_smmu_flush_range(uint32_t asid, uint64_t tablesPa, uint64_t tableBytes, uint64_t iova,
                                        uint64_t iovaBytes)
{
    if (!ready())
        return;
    IOSimpleLockLock(gLock);
    RegisterAccess io;
    switch_smmu::flushRange(io, asid, tablesPa, tableBytes, iova, iovaBytes);
    IOSimpleLockUnlock(gLock);
}

extern "C" uint32_t tegra_smmu_read(uint32_t offset)
{
    return ready() ? r32(offset) : 0;
}

extern "C" void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on)
{
    if (!ready())
        return;
    IOSimpleLockLock(gLock);
    const uint32_t value = r32(ctrl);
    const uint32_t wrote = on ? value | bit : value & ~bit;
    w32(ctrl, wrote);
    const uint32_t now = r32(ctrl);
    IOSimpleLockUnlock(gLock);
    if ((now ^ wrote) & bit) {
        IOLog("TegraSMMU: MC hot reset %s of %s did not take: MC %#x reads %#x after %#x was written\n",
              on ? "request" : "release", switch_mc::hotResetClient(ctrl, bit), ctrl, now, wrote);
    }
}

extern "C" int tegra_smmu_resume(void)
{
    if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE) || !ready())
        return 0;
    char failed[TEGRA_SMMU_ATTACHMENTS][96] = {};
    IOSimpleLockLock(gLock);
    const uint32_t count = gAttached.count;
    for (uint32_t i = 0; i < count; ++i) {
        bool seen = false;
        for (uint32_t j = 0; j < i; ++j)
            seen |= gAttached.entry[j].asidReg == gAttached.entry[i].asidReg;
        if (!seen)
            (void)programLocked(gAttached.entry[i], gAttached, failed[i], sizeof(failed[i]));
    }
    IOSimpleLockUnlock(gLock);
    int bad = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!failed[i][0])
            continue;
        IOLog("TegraSMMU: resume: %s\n", failed[i]);
        ++bad;
    }
    switch_mc_errors_resume();
    return bad;
}
