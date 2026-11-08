// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 MC SMMU registers under one lock (TegraSMMU.h). Short register sequences only: no allocation, no waits.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include "TegraSMMU.h"
#include "TegraSMMUAttachments.h"
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
bool gAttachedFull;                // under gLock: an attachment the table could not keep

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
        volatile uint8_t *mc = switch_mmio_map(kMcPA, 0x1000);
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        volatile uint8_t *expected = nullptr;
        if (!mc)
            return false;
        __atomic_compare_exchange_n(&gMc, &expected, mc, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return true;
}

// Lock held.
void flushLocked(uint32_t asid)
{
    w32(PTC_FLUSH, 0);  // all
    (void)r32(PTB_ASID);
    w32(TLB_FLUSH, TLB_FLUSH_ASID_MATCH | (asid << 24));  // this ASID, every address
    (void)r32(PTB_ASID);
}

// Lock held: the directory (read back), a flush, the clients' translation bits, the swgroup, a flush.
int programLocked(const tegra_smmu_attachment &a, char *line, size_t size)
{
    w32(PTB_ASID, a.asid);
    w32(PTB_DATA, a.ptb);
    (void)r32(PTB_ASID);
    w32(PTB_ASID, a.asid);
    if (r32(PTB_DATA) != a.ptb) {
        snprintf(line, size, "ASID %u directory did not take (PTB_DATA %#x, wrote %#x)", a.asid, r32(PTB_DATA), a.ptb);
        return -1;
    }
    flushLocked(a.asid);
    w32(a.translationReg, r32(a.translationReg) | a.translationBits);
    w32(a.asidReg, ASID_ENABLE | a.asid);
    (void)r32(a.asidReg);
    if (r32(a.asidReg) != (ASID_ENABLE | a.asid)) {
        snprintf(line, size, "swgroup ASID did not take (MC %#x = %#x)", a.asidReg, r32(a.asidReg));
        return -1;
    }
    flushLocked(a.asid);
    return 0;
}
}  // namespace

extern "C" int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                                 uint32_t translationBits, char *line, size_t size)
{
    if (!ready()) {
        snprintf(line, size, "cannot map the memory controller");
        return -1;
    }
    IOSimpleLockLock(gLock);
    int ret = -1;
    const uint32_t was = r32(asidReg);
    if ((was & ASID_ENABLE) && (was & 0x7f) != asid) {
        snprintf(line, size, "the swgroup is already translated by ASID %u (MC %#x = %#x)", was & 0x7f, asidReg, was);
        goto out;
    }
    {
        const tegra_smmu_attachment attachment = {asid, ptb, asidReg, translationReg, translationBits};
        ret = programLocked(attachment, line, size);
        if (!ret && !tegra_smmu_attachments_note(&gAttached, &attachment))
            gAttachedFull = true;
    }
out:
    IOSimpleLockUnlock(gLock);
    return ret;
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
}

extern "C" void tegra_smmu_flush(uint32_t asid)
{
    if (!ready())
        return;
    IOSimpleLockLock(gLock);
    flushLocked(asid);
    IOSimpleLockUnlock(gLock);
}

extern "C" uint32_t tegra_smmu_read(uint32_t offset)
{
    return ready() ? r32(offset) : 0;
}

extern "C" int tegra_smmu_resume(void)
{
    if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE) || !ready())
        return 0;
    char failed[TEGRA_SMMU_ATTACHMENTS][96] = {};
    IOSimpleLockLock(gLock);
    const uint32_t count = gAttached.count;
    const bool full = gAttachedFull;
    for (uint32_t i = 0; i < count; ++i)
        (void)programLocked(gAttached.entry[i], failed[i], sizeof(failed[i]));
    IOSimpleLockUnlock(gLock);
    int bad = full ? 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!failed[i][0])
            continue;
        IOLog("TegraSMMU: resume: %s\n", failed[i]);
        ++bad;
    }
    if (full)
        IOLog("TegraSMMU: resume: over %u swgroups were attached; one was not restored\n", TEGRA_SMMU_ATTACHMENTS);
    return bad;
}
