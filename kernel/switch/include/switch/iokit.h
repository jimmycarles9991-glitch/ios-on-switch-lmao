// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOKit hooks (kernel/switch/iokit/). Included by iokit/Kernel/IOUserClient.cpp and iokit/Kernel/IOLib.cpp.
 */
#ifndef SWITCH_IOKIT_H
#define SWITCH_IOKIT_H

#include <stdint.h>

#ifdef __cplusplus
#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>

/*
 * IOKernelAllocateWithPhysicalRestrict, after it has decided whether the allocation must be contiguous: one page under
 * a physical limit below the end of RAM goes to kmem_alloc_contig, which keeps the limit. XNU's own path for it drops
 * the limit and asks for KMA_LOMEM, which arm64 ignores: Apple's DMA masters sit behind DART, but there is no system
 * IOMapper here, so a device sees the physical address and a below-4 GiB buffer has to be below 4 GiB. With 16 KiB VM
 * pages every buffer of 16 KiB or less takes this path (a 4 KiB XUDC transfer ring landed above 4 GiB).
 */
static inline bool switch_iolib_contiguous(bool contiguous, uint64_t size, uint64_t maxPhys, uint64_t pageSize,
                                           uint64_t ramEnd)
{
    return contiguous || (maxPhys != 0 && maxPhys < ramEnd && size <= pageSize);
}

class IOUserClient;

/*
 * IOSurfaceLock and IOSurfaceUnlock are IOKit traps on IOSurfaceRootUserClient, not external methods: userland's
 * IOSurfaceClientLock/Unlock call IOConnectTrap3(connection, 2 or 3, surface ID, lock options, &seed).
 */
enum {
    kSwitchIOSurfaceTrapLock = 2,
    kSwitchIOSurfaceTrapUnlock = 3,
    kSwitchIOSurfaceLockReadOnly = 1, /* kIOSurfaceLockReadOnly, in the lock options */
};

void switch_iokit_iosurface_trap(IOUserClient *client, uint32_t index, void *p1, void *p2);
kern_return_t switch_iokit_iosurface_lock_will(IOUserClient *client, void *p1, void *p2);
void switch_iokit_iosurface_lock_failed(IOUserClient *client, void *p1, void *p2);
void switch_iokit_iosurface_ownership_done(IOUserClient *client, uint32_t index, void *p1, void *p2,
                                           kern_return_t result);
void switch_iokit_iosurface_method_done(IOUserClient *client, uint32_t selector,
                                        const struct IOExternalMethodArguments *args);

/* is_io_connect_method, after the client's method ran: an IOSurface create hands the new surface to the GPU driver,
 * which faults its pages in off the creating thread (tegra_gm20b_iosurface_method_done). */
static inline void switch_iokit_method_done(IOUserClient *client, uint32_t selector,
                                            const struct IOExternalMethodArguments *args, kern_return_t result)
{
    if (result == KERN_SUCCESS) {
        switch_iokit_iosurface_method_done(client, selector, args);
    }
}

/* iokit_user_client_trap, before the client's trap runs (result: the filter's so far): an IOSurface lock first waits
 * for the GPU writes it must see, or fails as Apple's does when kIOSurfaceLockAvoidSync forbids that wait; an unlock
 * cleans what the CPU wrote while the surface is still locked, so whoever sees the seed the unlock moves (Core Animation
 * reads a surface again when its seed moved) reads memory the GPU sees too. */
static inline kern_return_t switch_iokit_trap_will(IOUserClient *client, uint32_t index, void *p1, void *p2,
                                                   kern_return_t result)
{
    if (result == KERN_SUCCESS && index == kSwitchIOSurfaceTrapLock) {
        return switch_iokit_iosurface_lock_will(client, p1, p2);
    }
    if (result == KERN_SUCCESS && index == kSwitchIOSurfaceTrapUnlock) {
        switch_iokit_iosurface_trap(client, index, p1, p2);
    }
    return result;
}

/* iokit_user_client_trap, after the client's trap ran: a lock invalidates what the GPU wrote; a lock that failed after
 * the driver prepared it takes that back. kIOReturnCannotLock is the driver's own refusal (switch_iokit_trap_will),
 * which prepared nothing; IOSurface gives it only for kIOSurfaceLockAvoidSync on a surface whose use count says the GPU
 * uses it, which no driver here raises. */
static inline void switch_iokit_trap_done(IOUserClient *client, uint32_t index, void *p1, void *p2,
                                          kern_return_t result)
{
    switch_iokit_iosurface_ownership_done(client, index, p1, p2, result);
    if (result == KERN_SUCCESS && index == kSwitchIOSurfaceTrapLock) {
        switch_iokit_iosurface_trap(client, index, p1, p2);
    } else if (index == kSwitchIOSurfaceTrapLock && result != kIOReturnCannotLock) {
        switch_iokit_iosurface_lock_failed(client, p1, p2);
    }
}
#endif

#endif
