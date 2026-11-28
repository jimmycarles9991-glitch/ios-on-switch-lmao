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
// Whether an attachment of b's would translate through another's directory for one of a's ASIDs.
bool sharesAsid(const tegra_smmu_attachment &a, const tegra_smmu_attachment &b)
{
    for (uint32_t i = 0; i < tegra_smmu_attachment_fields(&a); ++i) {
        for (uint32_t j = 0; j < tegra_smmu_attachment_fields(&b); ++j) {
            if (tegra_smmu_attachment_asid(&a, i) == tegra_smmu_attachment_asid(&b, j) &&
                tegra_smmu_attachment_ptb(&a, i) != tegra_smmu_attachment_ptb(&b, j))
                return true;
        }
    }
    return false;
}

// Whether the swgroup register's value says another owner translates it. A swgroup that always translates reads
// enabled at reset, with ASID 0, and after our own detach, on the fault ASID: neither is an owner.
bool translatedElsewhere(const tegra_smmu_attachment &attachment, uint32_t was)
{
    if (attachment.alwaysTranslated) {
        return was != ASID_ENABLE && was != switch_smmu::parked(attachment, TEGRA_SMMU_ASID_FAULT) &&
               was != switch_smmu::selection(attachment);
    }
    return (was & ASID_ENABLE) && (was & 0x7f) != attachment.asid;
}

int attach(const tegra_smmu_attachment &attachment, char *line, size_t size)
{
    if (!ready()) {
        snprintf(line, size, "cannot map the memory controller");
        return -1;
    }
    IOSimpleLockLock(gLock);
    int ret = -1;
    const uint32_t was = r32(attachment.asidReg);
    const bool retained = tegra_smmu_attachments_find(&gAttached, attachment.asidReg) != UINT32_MAX;
    if (attachment.alwaysTranslated && !(was & ASID_ENABLE)) {
        // The MC behind these registers is not one whose GPUB always translates (an emulator's): a[34] = 1 accesses
        // would not go through any directory programmed here.
        snprintf(line, size, "the swgroup's enable reads 0 (MC %#x = %#x), but Tegra X1 hardwires it to 1",
                 attachment.asidReg, was);
    } else if (!retained && translatedElsewhere(attachment, was)) {
        snprintf(line, size, "the swgroup is already translated by ASID %u (MC %#x = %#x)", was & 0x7f,
                 attachment.asidReg, was);
    } else {
        bool conflict = false;
        for (uint32_t i = 0; i < gAttached.count; ++i) {
            const auto &owned = gAttached.entry[i];
            conflict |= sharesAsid(owned, attachment) || (owned.asidReg == attachment.asidReg &&
                                                          !tegra_smmu_attachment_same_directories(&owned, &attachment));
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
    return attach(tegra_smmu_attachment_of(asid, ptb, asidReg, translationReg, translationBits, false), line, size);
}

extern "C" int tegra_smmu_attach_wide(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                                      uint32_t translationBits, char *line, size_t size)
{
    return attach(tegra_smmu_attachment_of(asid, ptb, asidReg, translationReg, translationBits, true), line, size);
}

extern "C" int tegra_smmu_attach_split(const uint32_t asids[4], const uint32_t ptbs[4], uint32_t asidReg,
                                       uint32_t translationReg, uint32_t translationBits, bool alwaysTranslated,
                                       char *line, size_t size)
{
    tegra_smmu_attachment split =
        tegra_smmu_attachment_of(asids[0], ptbs[0], asidReg, translationReg, translationBits, false);
    split.alwaysTranslated = alwaysTranslated;
    for (uint32_t field = 1; field < 4; ++field) {
        if (!asids[field] || !ptbs[field]) {
            snprintf(line, size, "field %u of swgroup %#x names no ASID or directory", field, asidReg);
            return -1;
        }
        split.upperAsid[field - 1] = asids[field];
        split.upperPtb[field - 1] = ptbs[field];
    }
    return attach(split, line, size);
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
    const uint32_t at = tegra_smmu_attachments_find(&gAttached, asidReg);
    tegra_smmu_attachment gone = tegra_smmu_attachment_of(asid, 0, asidReg, 0, 0, false);
    if (at != UINT32_MAX)
        gone = gAttached.entry[at];
    tegra_smmu_attachments_forget(&gAttached, asidReg);
    w32(asidReg, switch_smmu::parked(gone, TEGRA_SMMU_ASID_FAULT));
    (void)r32(asidReg);
    for (uint32_t field = 0; field < tegra_smmu_attachment_fields(&gone); ++field) {
        const uint32_t fieldAsid = tegra_smmu_attachment_asid(&gone, field);
        if (!switch_smmu::firstUse(gone, field) || fieldAsid == TEGRA_SMMU_ASID_FAULT)
            continue;
        w32(PTB_ASID, fieldAsid);
        w32(PTB_DATA, 0);
        flushLocked(fieldAsid);
    }
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

extern "C" void switch_smmu_flush_tables(uint32_t asid, const switch_smmu_table_flush *tables, uint32_t count)
{
    if (!ready() || !count)
        return;
    IOSimpleLockLock(gLock);
    RegisterAccess io;
    switch_smmu::flushTables(io, asid, tables, count);
    IOSimpleLockUnlock(gLock);
}

#if DEVELOPMENT || DEBUG
namespace {
// Tenths of a percent.
uint32_t share(uint32_t part, uint32_t other)
{
    const uint64_t all = uint64_t(part) + other;
    return all ? static_cast<uint32_t>(uint64_t(part) * 1000 / all) : 0;
}
}  // namespace

extern "C" void switch_smmu_stats_window(char *line, size_t size)
{
    static switch_smmu::Stats last;  // the GPU's stats tick is the one caller, one window at a time
    if (!ready()) {
        snprintf(line, size, "no MC registers");
        return;
    }
    RegisterAccess io;
    IOSimpleLockLock(gLock);
    const uint32_t tlbConfig = r32(switch_smmu::kTlbConfig), ptcConfig = r32(switch_smmu::kPtcConfig);
    const bool counting = switch_smmu::statsOn(io);
    const switch_smmu::Stats now = switch_smmu::stats(io);
    IOSimpleLockUnlock(gLock);
    const switch_smmu::Stats d = {now.tlbHit - last.tlbHit, now.tlbMiss - last.tlbMiss, now.ptcHit - last.ptcHit,
                                  now.ptcMiss - last.ptcMiss};
    last = now;
    if (!counting) {
        snprintf(line, size, "counters on from here (TLB_CONFIG %#x PTC_CONFIG %#x)", tlbConfig, ptcConfig);
        return;
    }
    const uint32_t tlb = share(d.tlbHit, d.tlbMiss), ptc = share(d.ptcHit, d.ptcMiss);
    snprintf(line, size, "tlb %u hits %u misses (%u.%u%% hit), ptc %u hits %u misses (%u.%u%% hit)", d.tlbHit,
             d.tlbMiss, tlb / 10, tlb % 10, d.ptcHit, d.ptcMiss, ptc / 10, ptc % 10);
}
#endif

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
    // SC7's wake runs this once, on one thread: the lines need not be on its stack.
    static char failed[TEGRA_SMMU_ATTACHMENTS][96];
    for (char *line : failed)
        line[0] = 0;
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
