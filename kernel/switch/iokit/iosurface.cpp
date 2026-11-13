// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOSurface coherency (GPU <-> CPU). On Tegra the GPU does not snoop the CPU caches. At every process's IOSurfaceLock
 * and IOSurfaceUnlock (IOSurfaceRootUserClient traps 2 and 3), the TegraGM20B driver does the one cache op that
 * direction needs on the surfaces it imported (tegra_gm20b_iosurface_trap, TegraGM20BCoherency.cpp): a lock after GPU
 * writes invalidates, an unlock that wrote cleans (before IOSurface's unlock moves the seed). Before a lock runs, the
 * driver waits for the GPU's pending writes to the surface (tegra_gm20b_iosurface_lock_will). A block-linear surface's
 * lock gives the locking task a view of its texels as rows, and its unlock puts them back (tegra_gm20b_iosurface_view),
 * both before IOSurface's trap. Surfaces the GPU never imported cost a class compare and a hash lookup. A create's
 * output goes to the driver too, which faults a large new surface's pages in on a thread call, so the GPU import's
 * wiring finds them resident instead of zero-filling on the render thread (tegra_gm20b_iosurface_method_done).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <os/atomic_private.h>
#include <string.h>

#include <switch/iokit.h>

extern "C" void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options) __attribute__((weak));
extern "C" int tegra_gm20b_iosurface_lock_will(uint32_t surface_id, uint32_t options) __attribute__((weak));
extern "C" int switch_gpu_surface_lock_will(void *, uint32_t, uint32_t) __attribute__((weak));
extern "C" void switch_gpu_surface_lock_did(void *, uint32_t, uint32_t) __attribute__((weak));
extern "C" void switch_gpu_surface_lock_failed(void *, uint32_t, uint32_t) __attribute__((weak));
extern "C" void switch_gpu_surface_unlock_will(void *, uint32_t, uint32_t) __attribute__((weak));
extern "C" void switch_gpu_surface_unlock_did(void *, uint32_t, uint32_t) __attribute__((weak));
extern "C" void tegra_gm20b_iosurface_method_done(uint32_t selector, const void *output, uint32_t output_size,
                                                  task_t task) __attribute__((weak));
extern "C" int tegra_gm20b_iosurface_view(void *client, uint32_t index, uint32_t surface_id, uint32_t options)
    __attribute__((weak));

static bool surface_client(IOUserClient *client)
{
    static const OSMetaClass *surfaceClass;
    const OSMetaClass *meta = client->getMetaClass();

    if (meta != os_atomic_load(&surfaceClass, relaxed)) {
        if (os_atomic_load(&surfaceClass, relaxed) || !meta ||
            strcmp(meta->getClassName(), "IOSurfaceRootUserClient") != 0) {
            return false;
        }
        os_atomic_store(&surfaceClass, meta, relaxed);
    }
    return true;
}

void switch_iokit_iosurface_trap(IOUserClient *client, uint32_t index, void *p1, void *p2)
{
    if (surface_client(client)) {
        if (index == kSwitchIOSurfaceTrapLock && switch_gpu_surface_lock_did)
            switch_gpu_surface_lock_did(client, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
        if (index == kSwitchIOSurfaceTrapUnlock && switch_gpu_surface_unlock_will)
            switch_gpu_surface_unlock_will(client, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
    }
    if (index == kSwitchIOSurfaceTrapUnlock && tegra_gm20b_iosurface_view && surface_client(client)) {
        tegra_gm20b_iosurface_view(client, index, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
    }
    if (tegra_gm20b_iosurface_trap && surface_client(client)) {
        tegra_gm20b_iosurface_trap(index, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
    }
}

void switch_iokit_iosurface_ownership_done(IOUserClient *client, uint32_t index, void *p1, void *p2,
                                           kern_return_t result)
{
    if (index != kSwitchIOSurfaceTrapLock && index != kSwitchIOSurfaceTrapUnlock)
        return;
    if (!surface_client(client))
        return;
    if (index == kSwitchIOSurfaceTrapLock && result != KERN_SUCCESS && switch_gpu_surface_lock_failed)
        switch_gpu_surface_lock_failed(client, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
    if (index == kSwitchIOSurfaceTrapUnlock && result == KERN_SUCCESS && switch_gpu_surface_unlock_did)
        switch_gpu_surface_unlock_did(client, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
}

/* A lock IOSurface's trap refused, after the view hook prepared one: as if it were unlocked without a write. */
void switch_iokit_iosurface_lock_failed(IOUserClient *client, void *p1, void *p2)
{
    if (tegra_gm20b_iosurface_view && surface_client(client)) {
        tegra_gm20b_iosurface_view(client, kSwitchIOSurfaceTrapUnlock, (uint32_t)(uintptr_t)p1,
                                   (uint32_t)(uintptr_t)p2 | kSwitchIOSurfaceLockReadOnly);
    }
}

/* Apple's lock synchronises with the GPU: it waits for pending GPU writes, and with kIOSurfaceLockAvoidSync fails
 * with kIOReturnCannotLock instead of waiting. */
kern_return_t switch_iokit_iosurface_lock_will(IOUserClient *client, void *p1, void *p2)
{
    if (switch_gpu_surface_lock_will && surface_client(client) &&
        switch_gpu_surface_lock_will(client, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2))
        return kIOReturnCannotLock;
    if (tegra_gm20b_iosurface_view && surface_client(client) &&
        tegra_gm20b_iosurface_view(client, kSwitchIOSurfaceTrapLock, (uint32_t)(uintptr_t)p1,
                                   (uint32_t)(uintptr_t)p2)) {
        return kIOReturnCannotLock;
    }
    if (tegra_gm20b_iosurface_lock_will && surface_client(client) &&
        tegra_gm20b_iosurface_lock_will((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2)) {
        return kIOReturnCannotLock;
    }
    return KERN_SUCCESS;
}

void switch_iokit_iosurface_method_done(IOUserClient *client, uint32_t selector, const IOExternalMethodArguments *args)
{
    if (tegra_gm20b_iosurface_method_done && args->structureOutput && surface_client(client)) {
        tegra_gm20b_iosurface_method_done(selector, args->structureOutput, args->structureOutputSize, current_task());
    }
}
