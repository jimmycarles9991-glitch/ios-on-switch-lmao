// SPDX-License-Identifier: BSD-2-Clause-Patent
// One XUSB swgroup's address space through the MC SMMU, the same code for host mode (UsbSmmuMapper.cpp, XUSB_HOST) and
// device mode (SwitchXUDC, XUSB_DEV): its own ASID and directory (TegraSMMUDomain.h), RAM mapped once at fixed IOVAs
// (UsbStaticMap.h: all of it for the device, the low bank for the host), and a window of 4 KiB IOVA pages handed out
// from a rover (UsbIova.h). Map and unmap publish the changed page entries through TegraSMMUPublish.h while holding
// the space's lock, before handing out or reusing a window address. Both run in thread context.
#pragma once
#include <IOKit/IOLocks.h>
#if SWITCH_STATS_IO
#include <kern/clock.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "UsbIova.h"
#include "UsbStaticMap.h"
#include "XudcHardware.h"

class UsbSmmuSpace {
public:
    // RAM's banks from the device tree (/chosen/switch-ram-ledger). False with the reason.
    static bool ram(UsbRam *out, char *why, size_t whySize);
    // A directory for `asid` with `layout` installed: its fixed addresses and its window's tables. False with the
    // reason.
    bool init(uint32_t asid, const UsbStaticMap &layout, char *why, size_t whySize);
    // The swgroup to this directory (TegraSMMU.h tegra_smmu_attach; tegra_smmu_resume programs it again after SC7).
    bool attach(uint32_t asidReg, uint32_t translationReg, uint32_t translationBits, char *why, size_t whySize);

    bool attached() const
    {
        return attached_;
    }

    void lock()
    {
        IOLockLock(lock_);
    }

    void unlock()
    {
        IOLockUnlock(lock_);
    }

    // `pages` window pages, the first `alignPages`-aligned, page i at the physical page fill's mapPage(i, pa) gives it,
    // then one clean and one flush. The window index, or -1 (no room in the window, or fill refused: nothing kept).
    template <class Fill> int64_t mapUnlocked(uint32_t pages, uint32_t alignPages, Fill fill)
    {
#if SWITCH_STATS_IO
        const uint64_t entered = mach_absolute_time();
#endif
        int64_t index = window_.take(pages, alignPages);
        if (index >= 0) {
            const uint64_t base = window_.iova(static_cast<uint32_t>(index));
            const bool ok = fill([this, base](uint32_t i, uint64_t pa) {
                return domain_.map(base + static_cast<uint64_t>(i) * UsbIovaWindow::kPage, &pa, 1, true);
            });
            if (ok) {
                publish();
            } else {
                domain_.unmap(base, pages);
                window_.give(static_cast<uint32_t>(index), pages);
                index = -1;
            }
        }
#if SWITCH_STATS_IO
        if (index >= 0) {
            ++cost_.maps;
            cost_.mapTicks += mach_absolute_time() - entered;
        }
#endif
        return index;
    }

    template <class Fill> int64_t map(uint32_t pages, uint32_t alignPages, Fill fill)
    {
        lock();
        const int64_t index = mapUnlocked(pages, alignPages, fill);
        unlock();
        return index;
    }

    // What map() / mapUnlocked() returned, once the controller can no longer reach it: one clean and one flush.
    void unmapUnlocked(uint32_t index, uint32_t pages);
    void unmap(uint32_t index, uint32_t pages);

    uint64_t iova(uint32_t index) const
    {
        return window_.iova(index);
    }

    uint32_t index(uint64_t address) const
    {
        return window_.index(address);
    }

    uint32_t windowPages() const
    {
        return window_.pages;
    }

    // The fixed IOVA of [pa, pa + length); UINT64_MAX when it is not in one mapped bank of RAM.
    uint64_t fixedIova(uint64_t pa, uint64_t length) const
    {
        return map_.iova(pa, length);
    }

    bool owns(uint64_t address) const
    {
        return attached_ && window_.contains(address);
    }

    // The physical address a device address translates to (UINT64_MAX: unmapped).
    uint64_t translate(uint64_t address);

#if SWITCH_STATS_IO
    // The table entry of a window address's page, as the SMMU reads it (0: none).
    uint32_t entry(uint64_t address)
    {
        const uint32_t *pte = window_.contains(address) ? domain_.pte(address, false) : nullptr;
        return pte ? *pte : 0;
    }

    // Time in mach_absolute_time ticks for map() and for table publication in map and unmap.
    struct Cost {
        uint64_t maps, mapTicks, flushes, flushTicks;
    };

    Cost cost() const
    {
        return cost_;
    }
#endif

private:
    void publish();

    IOLock *lock_ = nullptr;
    xudc::DmaBuffer tables_;
    TegraSMMUDomain domain_;
    UsbIovaWindow window_;
    UsbStaticMap map_ = {};
    uint32_t asid_ = 0;
    bool attached_ = false;
#if SWITCH_STATS_IO
    Cost cost_ = {};
#endif
};
