// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes SwitchAccelStub.cpp's accelerator from bsd_init, long before backboardd starts: load IOGPUFamily (prelinked;
// nothing else loads it) so the IOAcceleratorES metaclass exists, register our subclass (switch_kernel_classes), then
// attach one registered, never-started instance to the platform expert. Same staging as TegraGM20B.cpp's publish() and
// UniXNU-qemu's QemuMetalPublish.cpp. Registering is what IOServiceGetMatchingServices needs (kIOServiceMatchedState,
// IOService::getMatchingServices). Any failure is a build or ABI mismatch, so it panics instead of costing 30 s per boot.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSMetaClass.h>

extern "C" {
OSReturn switch_kernel_classes(void (*initialize)());
void switch_accel_stub_static_init(void) __asm__("__GLOBAL__sub_I_SwitchAccelStub.cpp");
IOService *switch_accel_stub_allocate(void);
unsigned switch_accel_stub_instance_size(void);
void switch_accel_stub_publish(void);
}

void switch_accel_stub_publish(void)
{
    // Unless switch-no-gpu, drivers/TegraGM20B publishes the real AGXAccelerator. The stub would end QuartzCore's poll
    // before that exists and Metal would cache an empty device list for backboardd's lifetime, so it stays out of the
    // way then.
    unsigned noGpu = 0;
    if (!(PE_parse_boot_argn("switch-no-gpu", &noGpu, sizeof(noGpu)) && noGpu))
        return;
    const kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (loaded)
        panic("SwitchAccelStub: IOGPUFamily load failed 0x%x", loaded);
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136 || switch_accel_stub_instance_size() != 136) {
        panic("SwitchAccelStub: IOAcceleratorES %p size %u, ours %u", parent, parent ? parent->getClassSize() : 0,
              switch_accel_stub_instance_size());
    }
    const OSReturn registered = switch_kernel_classes(switch_accel_stub_static_init);
    if (registered)
        panic("SwitchAccelStub: registering its class failed: 0x%x", registered);
    IOService *accelerator = switch_accel_stub_allocate();
    if (!accelerator || !accelerator->init())
        panic("SwitchAccelStub: allocation failed");
    if (!accelerator->attach(IOService::getPlatform()))
        panic("SwitchAccelStub: attach to the platform failed");
    accelerator->registerService();
    accelerator->release();
}
