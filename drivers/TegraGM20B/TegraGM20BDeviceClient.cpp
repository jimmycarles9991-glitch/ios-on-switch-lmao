// SPDX-License-Identifier: MIT
// IOGPU's device connection on AGXAccelerator: IOGPUDeviceCreate (IOGPU.framework) opens type 1 (API in the high half)
// and QuartzCore's IOMFBDisplay::hint_agx_timing then gives the GPU each vsync's deadline through it
// (IOGPUDeviceSetDisplayParams). It holds no GPU resources: IOGPUDeviceCreate's queries are answered empty (no shared
// stamps, no trace ids, as a device with none) and the deadline is kept for the scheduler (tegra_gm20b_display_deadline).
#include <IOKit/IOUserClient.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace
{
// IOGPUDeviceCreateWithAPIProperty's struct selectors (input, output) and IOGPUDeviceSetDisplayParams' scalar one.
enum : uint32_t {
	kDeviceInfo = 0,        // 0x40 bytes out
	kSharedMemoryInfo = 2,  // 0x218 bytes out: base, offsets and their count (0: none)
	kTraceInfo = 4,         // 0x20 bytes out
	kClientName = 6,        // 16 bytes in: the process's name
	kSetDisplayParams = 0x21,
};

// The latest deadline QuartzCore gave (a seqlock: odd while written): the next vblank and the frame interval in mach
// absolute time units, as its vsync callback computes them.
struct Deadline {
	volatile uint32_t sequence;
	volatile uint64_t nextVblank, interval;
} gDeadline;

void storeDeadline(uint64_t nextVblank, uint64_t interval)
{
	__atomic_add_fetch(&gDeadline.sequence, 1, __ATOMIC_ACQ_REL);
	gDeadline.nextVblank = nextVblank;
	gDeadline.interval = interval;
	__atomic_add_fetch(&gDeadline.sequence, 1, __ATOMIC_RELEASE);
}
}  // namespace

// The deadline QuartzCore last gave; false before the first.
extern "C" bool tegra_gm20b_display_deadline(uint64_t *nextVblank, uint64_t *interval)
{
	uint32_t before, after;
	uint64_t next, step;
	do {
		before = __atomic_load_n(&gDeadline.sequence, __ATOMIC_ACQUIRE);
		next = gDeadline.nextVblank;
		step = gDeadline.interval;
		// An acquire load orders only what follows it: without the fence the two reads above may complete after the
		// second sequence load and pass the check torn.
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(&gDeadline.sequence, __ATOMIC_RELAXED);
	} while ((before & 1) || before != after);
	if (!before)
		return false;
	*nextVblank = next;
	*interval = step;
	return true;
}

class TegraGM20BDeviceUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(TegraGM20BDeviceUserClient);

public:
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
				IOExternalMethodDispatch *dispatch, OSObject *target,
				void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(TegraGM20BDeviceUserClient, IOUserClient)

	IOReturn TegraGM20BDeviceUserClient::clientClose()
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn TegraGM20BDeviceUserClient::externalMethod(uint32_t selector,
						    IOExternalMethodArguments *arguments,
						    IOExternalMethodDispatch *, OSObject *, void *)
{
	auto answerEmpty = [arguments](uint32_t size) {
		if (arguments->structureOutputDescriptor || arguments->structureOutputSize < size ||
		    !arguments->structureOutput)
			return kIOReturnBadArgument;
		bzero(arguments->structureOutput, size);
		arguments->structureOutputSize = size;
		return kIOReturnSuccess;
	};
	switch (selector) {
	case kDeviceInfo:
		return answerEmpty(0x40);
	case kSharedMemoryInfo:
		return answerEmpty(0x218);
	case kTraceInfo:
		return answerEmpty(0x20);
	case kClientName:
		return arguments->structureInputSize <= 16 ? kIOReturnSuccess
							   : kIOReturnBadArgument;
	case kSetDisplayParams:
		if (arguments->scalarInputCount != 2)
			return kIOReturnBadArgument;
		storeDeadline(arguments->scalarInput[0], arguments->scalarInput[1]);
		return kIOReturnSuccess;
	default:
		switch_refusal_k(switch_refusal_key_with(
					 switch_refusal_key("iogpu", "device-selector"), selector),
				 "iogpu", "device-selector", "IOGPU device selector %u refused",
				 selector);
		return kIOReturnUnsupported;
	}
}

// Called by tegra_gm20b_new_user_client for IOGPU's device type.
extern "C" IOReturn tegra_gm20b_new_device_client(IOService *provider, task_t task,
						  void *securityID, UInt32 type,
						  OSDictionary *properties, IOUserClient **handler)
{
	TegraGM20BDeviceUserClient *client = OSTypeAlloc(TegraGM20BDeviceUserClient);
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(task, securityID, type, properties) ||
	    !client->attach(provider)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(provider)) {
		client->detach(provider);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}
