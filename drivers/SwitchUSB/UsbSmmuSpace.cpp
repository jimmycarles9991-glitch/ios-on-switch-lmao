// SPDX-License-Identifier: BSD-2-Clause-Patent
// See UsbSmmuSpace.h.
#include "UsbSmmuSpace.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "../TegraSMMU/TegraSMMUPublish.h"

bool UsbSmmuSpace::ram(UsbRam *out, char *why, size_t whySize)
{
    const IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    const OSData *ledger = chosen ? switch_osdata(chosen->getProperty("switch-ram-ledger")) : nullptr;
    const bool read = ledger != nullptr && UsbRam::fromLedger(ledger->getBytesNoCopy(), ledger->getLength(), out);
    OSSafeReleaseNULL(chosen);
    if (!read)
        snprintf(why, whySize, "no committed RAM ledger in /chosen");
    return read;
}

bool UsbSmmuSpace::init(uint32_t asid, const UsbStaticMap &layout, char *why, size_t whySize)
{
    if (!layout.fits()) {
        snprintf(why, whySize, "RAM %#llx-%#llx and %#llx-%#llx does not fit one 32-bit space beside the window",
                 layout.ram.lowStart, layout.ram.lowEnd, layout.ram.highStart, layout.ram.highEnd);
        return false;
    }
    const uint32_t tablePages = 1 + layout.tables();
    const auto windowPages = static_cast<uint32_t>(layout.windowBytes / UsbIovaWindow::kPage);
    lock_ = IOLockAlloc();
    uint8_t *used = IONewZero(uint8_t, windowPages ? windowPages : 1);
    if (!lock_ || !used || !tables_.allocate(tablePages * static_cast<uint32_t>(TegraSMMUDomain::kPage)) ||
        !domain_.init(tables_.va, tables_.pa, tablePages)) {
        snprintf(why, whySize, "no memory for the SMMU tables");
        return false;
    }
    asid_ = asid;
    if (!layout.install(domain_)) {
        snprintf(why, whySize, "the SMMU tables cannot hold RAM's map");
        return false;
    }
    window_.init(used, windowPages, layout.windowBase);
    map_ = layout;
    return true;
}

bool UsbSmmuSpace::attach(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, char *why,
                          size_t whySize)
{
    switch_dcache_clean_invalidate(tables_.va, tables_.size);
    uintptr_t low = 0;
    uintptr_t high = 0;
    (void)domain_.takeDirty(&low, &high);
    if (tegra_smmu_attach_wide(asid_, domain_.ptb(), asidReg, translationReg, translationBits, why, whySize))
        return false;
    attached_ = true;
    return true;
}

void UsbSmmuSpace::unmap(uint32_t index, uint32_t pages)
{
    IOLockLock(lock_);
    domain_.unmap(window_.iova(index), pages);
    publish();
    window_.give(index, pages);
    IOLockUnlock(lock_);
}

uint64_t UsbSmmuSpace::translate(uint64_t address)
{
    if (!window_.contains(address))
        return map_.pa(address);
    IOLockLock(lock_);
    const uint64_t pa = domain_.translate(address);
    IOLockUnlock(lock_);
    return pa;
}

void UsbSmmuSpace::publish()
{
#if DEVELOPMENT || DEBUG
    const uint64_t entered = mach_absolute_time();
#endif
    switch_smmu_publish(domain_, asid_, SWITCH_DCACHE_CLEAN_INVALIDATE);
#if DEVELOPMENT || DEBUG
    ++cost_.flushes;
    cost_.flushTicks += mach_absolute_time() - entered;
#endif
}
