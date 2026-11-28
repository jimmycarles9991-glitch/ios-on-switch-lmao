// SPDX-License-Identifier: MIT
// Debug builds: what a process keeps once the GPU's idle memory went back, as jetsam counts it (phys_footprint, against a
// daemon's limits: iconservicesagent's InactiveHard is 15 MB), by the kernel's own ledger entries, each named with what
// of the Metal driver lies in it.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPassCache.h"

#include <dlfcn.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <unistd.h>

#if SWITCH_METAL_STATS
namespace {

// The ledger system call's records (XNU osfmk/kern/ledger.h, bsd/kern/sys_generic.c ledger())
constexpr int kLedgerEntryInfo = 1, kLedgerTemplateInfo = 2, kLedgerNameMax = 32;

struct LedgerTemplateInfo {
    char name[kLedgerNameMax], group[kLedgerNameMax], units[kLedgerNameMax];
};

struct LedgerEntryInfo {
    int64_t balance, credit, debit;
    uint64_t limit, refillPeriod, lastRefill;
};

// phys_footprint's terms (XNU task.c init_task_ledgers): internal and compressed less what IOKit mappings hold of them,
// iokit_mapped, purgeable non-volatile memory and page tables
struct Footprint {
    int64_t footprint = 0, internal = 0, compressed = 0, iokit = 0, alternate = 0, alternateCompressed = 0,
            purgeable = 0, pageTable = 0;
    uint64_t limit = 0;  // jetsam's limit on phys_footprint (its ledger entry's), 0: none
};

// A ledger entry's limit when none is set (XNU ledger.h LEDGER_LIMIT_INFINITY)
constexpr uint64_t kNoLimit = (1ULL << 63) - 1;

bool ledgerFootprint(Footprint &f)
{
    static std::atomic<bool> refused{false};
    if (refused.load(std::memory_order_relaxed))
        return false;
    using Ledger = int (*)(int, caddr_t, caddr_t, caddr_t);
    static const auto ledger = reinterpret_cast<Ledger>(dlsym(RTLD_DEFAULT, "ledger"));
    if (!ledger)
        return false;
    std::vector<LedgerTemplateInfo> names(256);
    int count = int(names.size());
    if (ledger(kLedgerTemplateInfo, reinterpret_cast<caddr_t>(names.data()), reinterpret_cast<caddr_t>(&count),
               nullptr)) {
        if (errno == EPERM || errno == EACCES || errno == ENOSYS)
            refused.store(true, std::memory_order_relaxed);
        return false;
    }
    std::vector<LedgerEntryInfo> entries(size_t(std::max(count, 0)));
    int entryCount = count;
    if (ledger(kLedgerEntryInfo, reinterpret_cast<caddr_t>(intptr_t(getpid())),
               reinterpret_cast<caddr_t>(entries.data()), reinterpret_cast<caddr_t>(&entryCount))) {
        if (errno == EPERM || errno == EACCES || errno == ENOSYS)
            refused.store(true, std::memory_order_relaxed);
        return false;
    }
    const std::pair<const char *, int64_t *> wanted[] = {
        {"phys_footprint", &f.footprint},        {"internal", &f.internal},
        {"internal_compressed", &f.compressed},  {"iokit_mapped", &f.iokit},
        {"alternate_accounting", &f.alternate},  {"alternate_accounting_compressed", &f.alternateCompressed},
        {"purgeable_nonvolatile", &f.purgeable}, {"page_table", &f.pageTable},
    };
    for (int i = 0; i < std::min(count, entryCount); i++) {
        for (const auto &[name, value] : wanted)
            if (!strncmp(names[size_t(i)].name, name, kLedgerNameMax))
                *value = entries[size_t(i)].balance;
        if (!strncmp(names[size_t(i)].name, "phys_footprint", kLedgerNameMax))
            f.limit = entries[size_t(i)].limit == kNoLimit ? 0 : entries[size_t(i)].limit;
    }
    return f.footprint > 0;
}

// Without the ledger (a sandbox refuses it): task_info's terms, IOKit and page tables together
Footprint taskFootprint()
{
    Footprint f;
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
        return f;
    f.footprint = int64_t(vm.phys_footprint);
    f.internal = int64_t(vm.internal);
    f.compressed = int64_t(vm.compressed);
    f.iokit = std::max<int64_t>(0, f.footprint - f.internal - f.compressed);
    if (vm.limit_bytes_remaining > 0)
        f.limit = uint64_t(f.footprint + vm.limit_bytes_remaining);
    return f;
}

unsigned long long kb(int64_t bytes)
{
    return (unsigned long long)(std::max<int64_t>(bytes, 0) >> 10);
}

// The shader compiler's store as this process maps it (Remote::map: an IOKit mapping of the compiler's MAP_SHARED file).
// XNU charges an IOKit mapping to iokit_mapped, at its whole size, only when its VM object is internal (vm_map.c
// vm_map_enter: xnu-10063 3224-3242, xnu-12377 3748-3766); the store's object is its file's, whose resident pages are
// external and outside phys_footprint. `external`: every region of it is backed by an external pager.
struct StoreRegion {
    uint64_t mapped = 0, resident = 0;
    bool external = true;
};

StoreRegion storeRegion()
{
    StoreRegion r;
    if (!gRM->completion)
        return r;
    const uint8_t *base = gRM->completion->compilerStoreMapping(r.mapped);
    static std::atomic<bool> refused{false};
    if (refused.load(std::memory_order_relaxed))
        return r;
    for (vm_address_t at = vm_address_t(uintptr_t(base)); base && at < uintptr_t(base) + r.mapped;) {
        vm_address_t address = at;
        vm_size_t size = 0;
        vm_region_extended_info_data_t info = {};
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        const kern_return_t kr = vm_region_64(mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO,
                                              reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (kr != KERN_SUCCESS) {
            if (kr != KERN_INVALID_ADDRESS)
                refused.store(true, std::memory_order_relaxed);
            break;
        }
        if (address > at)
            break;
        r.resident += uint64_t(info.pages_resident) * vm_page_size;
        r.external = r.external && info.external_pager;
        at = address + size;
    }
    return r;
}

}  // namespace

// The tick's last line: a new one when what the device holds moved by a quarter MiB or the footprint by a MiB
constexpr int64_t kHeldStep = 256 << 10, kFootprintStep = 1 << 20;

void gm20bMemoryTick()
{
    static int64_t lastHeld = -kHeldStep, lastFootprint = -kFootprintStep;
    static uint64_t lastLimit = ~0ULL;
    gm20b::Device::Held h;
    if (!gRM || !gRM->raw->held(h, false))
        return;
    Footprint f;
    if (!ledgerFootprint(f))
        f = taskFootprint();
    const int64_t held = int64_t(h.chunks + h.writeCombined + h.heaps + h.kept + h.tables);
    if (std::abs(held - lastHeld) < kHeldStep && std::abs(f.footprint - lastFootprint) < kFootprintStep &&
        f.limit == lastLimit)
        return;
    lastHeld = held;
    lastFootprint = f.footprint;
    lastLimit = f.limit;
    char limit[48] = "of no limit";
    if (f.limit)
        snprintf(limit, sizeof limit, "of limit %llu KB", (unsigned long long)(f.limit >> 10));
    os_log(gm20bLog(),
           "SwitchMetal: memory held %llu KB: chunks %llu KB in %u (pinned %llu KB in %u, kept for reuse %llu KB), "
           "write-combined %llu KB, heaps %llu KB, kept imports %llu KB, tables %llu KB; GPU-only heaps %llu KB, "
           "volatile heaps %llu KB; parked chunks %llu KB in %u, heaps %llu KB in %u; footprint %llu KB %s, "
           "IOKit %llu KB",
           kb(held), kb(int64_t(h.chunks)), h.chunkCount, kb(int64_t(h.pinned)), h.pinnedCount, kb(int64_t(h.reuse)),
           kb(int64_t(h.writeCombined)), kb(int64_t(h.heaps)), kb(int64_t(h.kept)), kb(int64_t(h.tables)),
           kb(int64_t(h.gpuHeaps)), kb(int64_t(h.volatileHeaps)), kb(int64_t(h.parkedChunks)), h.parkedChunkCount,
           kb(int64_t(h.parkedHeaps)), h.parkedHeapCount, kb(f.footprint), limit, kb(f.iokit));
}

void logGivenBack(size_t relieved, const SwitchMetalPassTrim &passes, const gm20b::Device::Trimmed &kept,
                  const gm20b::Device::PurgeStats &purge)
{
    Footprint f;
    const bool ledgered = ledgerFootprint(f);
    if (!ledgered)
        f = taskFootprint();
    gm20b::Device &d = *gRM->raw;
    malloc_statistics_t heap = {};
    malloc_zone_statistics(nullptr, &heap);
    const int64_t pool = int64_t(d.stats.poolBytes.load()), copies = int64_t(d.heapBytes());
    const int64_t rest = f.internal - f.alternate - int64_t(heap.size_allocated) - pool - copies;
    gm20b::Device::Held held;
    d.held(held, true);
    const int64_t chunksMapped = int64_t(held.chunks);
    const int64_t iokitClaimed = int64_t(d.wcChunkBytesHeld() + d.controlMappedBytes()) + chunksMapped;
    const StoreRegion store = storeRegion();
    uint64_t arenaPlaced = 0, arenaVolatile = 0, arenaGiven = 0;
    d.arenaUse(arenaPlaced, arenaVolatile, &arenaGiven);
    os_log(
        gm20bLog(),
        "SwitchMetal: idle memory given back: footprint %llu KB = internal %llu KB (malloc %llu KB in use of %llu, raw "
        "pool "
        "%llu KB in %llu chunks with heaps %llu KB (%llu KB placed, %llu KB volatile, %llu KB given back), shader heap "
        "copies %llu KB, "
        "neither malloc's "
        "nor the device's %llu KB) + compressed %llu KB + IOKit %llu KB (raw chunks mapped %llu KB, raw "
        "write-combined %llu KB, raw tables and rings %llu KB, no owner %llu KB; %llu KB of it internal too) + "
        "purgeable %llu KB + page tables %llu "
        "KB%s; shader heap in place %llu KB (file pages: no footprint); compiler store mapped %llu KB (external pager "
        "%d, resident %llu KB); malloc gave back %llu KB; trim let go %zu kept surface imports (%llu KB) and %zu pass "
        "copies naming %zu textures; volatile purge %u (%llu KB), kept %u (%llu KB: committed %u/%u, leases %u, "
        "in-flight %u, busy %u, non-empty %u); parked chunks %llu KB in %u, heaps %llu KB in %u",
        kb(f.footprint), kb(f.internal - f.alternate), kb(int64_t(heap.size_in_use)), kb(int64_t(heap.size_allocated)),
        kb(pool), (unsigned long long)d.stats.poolChunks.load(), kb(int64_t(d.arenaBytes())), kb(int64_t(arenaPlaced)),
        kb(int64_t(arenaVolatile)), kb(int64_t(arenaGiven)), kb(copies), kb(rest),
        kb(f.compressed - f.alternateCompressed), kb(f.iokit), kb(chunksMapped), kb(int64_t(d.wcChunkBytesHeld())),
        kb(int64_t(d.controlMappedBytes())), kb(f.iokit - iokitClaimed), kb(f.alternate + f.alternateCompressed),
        kb(f.purgeable), kb(f.pageTable), ledgered ? "" : " (no ledger: IOKit counts page tables too)",
        kb(int64_t(d.heapInPlaceBytes())), kb(int64_t(store.mapped)), int(store.external), kb(int64_t(store.resident)),
        kb(int64_t(relieved)), kept.keptImports, kb(int64_t(kept.keptImportBytes)), passes.passes, passes.textures,
        purge.purged, kb(int64_t(purge.purgedBytes)), purge.skipped, kb(int64_t(purge.skippedBytes)), purge.committed,
        purge.committedCount, purge.leases, purge.inFlight, purge.busy, purge.nonEmpty, kb(int64_t(held.parkedChunks)),
        held.parkedChunkCount, kb(int64_t(held.parkedHeaps)), held.parkedHeapCount);
}
#endif
