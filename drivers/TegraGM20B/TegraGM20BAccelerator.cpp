// SPDX-License-Identifier: MIT
// The registry-visible accelerator for Metal discovery: MTLRegisterDevices iterates IOServiceMatching("IOAcceleratorES"),
// reads MetalPluginName/MetalPluginClassName, loads /System/Library/Extensions/<MetalPluginName>.bundle and sends
// -initWithAcceleratorPort: to that class (an _MTLDevice subclass); repos oosexgang_hackios/userland/src/metal_test.m:12-20,
// NWylynko_ios-flame/HANDOFF.md:741-746. Modelled on drivers/NativeAdreno/NativeAdrenoAccelerator.cpp.
//
// It derives from the original IOAcceleratorES of IOGPUFamily (prelinked; drivers/NativeMetal/OriginalAcceleratorABI.h), so it
// is a late object (kernel_driver's `late`): its static initializer is taken out of __mod_init_func and exported as
// __GLOBAL__sub_I_TegraGM20BAccelerator.cpp, which TegraGM20B.cpp runs once IOGPUFamily has loaded. Its three IOAcceleratorES
// imports (gMetaClass, the C2 constructor, the D2 destructor) are bound by drivers/SwitchAccelStub/original-bindings.S,
// which is in the same kernel link; this driver carries no bindings of its own.
//
// The accelerator keeps the Apple class name AGXAccelerator and its connection is AGXDeviceUserClient, so the original
// sandbox profiles that name them admit the render server's open, and NVK finds it by that class name (never the stub).
#include "../NativeMetal/OriginalAcceleratorABI.h"
#include <IOKit/IOUserClient.h>

extern "C" IOReturn tegra_gm20b_new_user_client(IOService *, task_t, void *, UInt32, OSDictionary *,
						IOUserClient **);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AGXAccelerator : public IOAcceleratorES
{
	// NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(AGXAccelerator) public
    : IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
			     OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES)

	IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
					       OSDictionary *properties, IOUserClient **handler)
{
	return tegra_gm20b_new_user_client(this, owningTask, securityID, type, properties, handler);
}

extern "C" IOService *tegra_gm20b_accelerator_allocate(void)
{
	return new AGXAccelerator;
}

extern "C" unsigned tegra_gm20b_accelerator_instance_size(void)
{
	return sizeof(AGXAccelerator);
}
