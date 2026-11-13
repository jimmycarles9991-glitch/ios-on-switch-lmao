// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOUserClient.h>
#include <string.h>

// An asynchronous call's reference, which carries a send right to its wake port. Until the method returns, the right
// belongs to the MIG request: on an error the server destroys the request and releases the right itself (ipc_kobject.c,
// ipc_kobject_server), so a method owns the right only by succeeding. settle() is given the method's result: a success
// releases the right unless the method took it, an error leaves it to MIG. Releasing it on an error as well drops one
// send right too many, the panic "send right count overflow (delta: -1)".
class SwitchGpuAsyncReference
{
public:
	explicit SwitchGpuAsyncReference(const IOExternalMethodArguments *arguments)
	{
		if (arguments && arguments->asyncReference && arguments->asyncReferenceCount) {
			const uint32_t count = arguments->asyncReferenceCount < kOSAsyncRef64Count
						       ? arguments->asyncReferenceCount
						       : kOSAsyncRef64Count;
			memcpy(reference_, arguments->asyncReference,
			       count * sizeof(reference_[0]));
		}
	}

	SwitchGpuAsyncReference(const SwitchGpuAsyncReference &) = delete;
	SwitchGpuAsyncReference &operator=(const SwitchGpuAsyncReference &) = delete;

	IOReturn settle(IOReturn result)
	{
		if (result == kIOReturnSuccess)
			IOUserClient::releaseAsyncReference64(reference_);
		reference_[0] = 0;
		return result;
	}

	// The right goes with the reference. A method that fails after taking it drops `output` unreleased (MIG releases
	// the right); one that succeeds leaves the one release to whatever holds `output`.
	void take(OSAsyncReference64 output)
	{
		memcpy(output, reference_, sizeof(reference_));
		reference_[0] = 0;
	}

private:
	OSAsyncReference64 reference_ = {};
};
