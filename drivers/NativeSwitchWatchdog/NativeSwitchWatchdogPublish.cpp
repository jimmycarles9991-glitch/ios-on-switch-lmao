// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes NativeSwitchWatchdog.cpp's IOWatchdog and PMU from the board thread (SwitchRootDiag.h), long before
// launchd: load AppleARMWatchdogTimer (prelinked; apfs depends on it) so the IOWatchdog metaclass exists, register our
// classes inside preModLoad/postModLoad, then attach one instance of each to IOResources and start it (the original start
// registers the watchdog). Same staging as SwitchAccelStubPublish.cpp. Only when both started does
// switch_watchdog_ready() say so, and kern_exec.c's launchd policy leaves watchdogd enabled; a failure is logged once
// and watchdogd stays disabled for the boot.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include "OriginalIOWatchdog.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void switch_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_NativeSwitchWatchdog.cpp");
IOService *switch_watchdog_allocate(void);
IOService *switch_pmu_allocate(void);
int switch_watchdog_publish(void);
int switch_watchdog_ready(void);
}

static volatile int watchdog_ready;  // both services started (read by launchd's exec, kern_exec.c)

int switch_watchdog_ready(void)
{
    return __atomic_load_n(&watchdog_ready, __ATOMIC_ACQUIRE);
}

int switch_watchdog_publish(void)
{
    const kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    const OSSymbol *name = OSSymbol::withCString("IOWatchdog");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (loaded || parent != &IOWatchdog::gMetaClass || parent->getClassSize() != sizeof(IOWatchdog)) {
        IOLog("SwitchWatchdog: AppleARMWatchdogTimer load 0x%x, IOWatchdog %p size %u\n", loaded, parent,
              parent ? parent->getClassSize() : 0);
        return -1;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle)
        return -1;
    switch_watchdog_static_init();
    if (OSMetaClass::postModLoad(handle))
        return -1;
    IOService *provider = IOService::getResourceService();
    IOService *watchdog = switch_watchdog_allocate();
    if (!provider || !watchdog || !watchdog->init() || !watchdog->attach(provider)) {
        OSSafeReleaseNULL(watchdog);
        return -1;
    }
    // Never released: once the original start ran, the IOWatchdog is the kext's global and watchdogd's provider.
    if (!watchdog->start(provider)) {
        IOLog("SwitchWatchdog: start failed\n");
        return -1;
    }
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): the started watchdog is kept for good (above)
    IOService *pmu = switch_pmu_allocate();
    bool attached = false;
    if (pmu && pmu->init()) {
        attached = pmu->attach(provider);
        if (attached && pmu->start(provider)) {
            pmu->release();  // the registry (IOResources) holds it
            __atomic_store_n(&watchdog_ready, 1, __ATOMIC_RELEASE);
            return 0;
        }
    }
    if (attached)
        pmu->detach(provider);
    OSSafeReleaseNULL(pmu);
    IOLog("SwitchWatchdog: NativeSwitchPMU did not start: watchdogd stays disabled\n");
    return -1;
}
