// SPDX-License-Identifier: MIT
#pragma once

IOReturn AGXDeviceUserClient::resourceObserve(const TegraGM20BAbiResourceObserve *input,
					      IOExternalMethodArguments *arguments,
					      SwitchGpuAsyncReference &reference)
{
	if (!input->cookie || input->reserved ||
	    (input->flags & ~uint32_t(TegraGM20BResourceObserveCancel)))
		return kIOReturnBadArgument;
	if (input->flags == TegraGM20BResourceObserveCancel) {
		if (input->uses_address || input->use_count || arguments->asyncReferenceCount)
			return kIOReturnBadArgument;
		return ioreturn(resources_.cancel(input->cookie));
	}
	if (!input->uses_address || !input->use_count || input->use_count > TegraGM20BMaxJobUses ||
	    !arguments->asyncReference || arguments->asyncReferenceCount < 3 ||
	    arguments->asyncReferenceCount > kOSAsyncRef64Count || !arguments->asyncWakePort)
		return kIOReturnBadArgument;
	auto *uses = IONew(TegraGM20BAbiSurfaceUse, input->use_count);
	auto *resolved = IONew(SwitchGpuResourceUse, input->use_count * 2);
	auto *notification = IONew(EventNotification, 1);
	if (!uses || !resolved || !notification) {
		IODelete(uses, TegraGM20BAbiSurfaceUse, input->use_count);
		IODelete(resolved, SwitchGpuResourceUse, input->use_count * 2);
		IODelete(notification, EventNotification, 1);
		return kIOReturnNoMemory;
	}
	IOReturn result = copyin(input->uses_address, uses, input->use_count * sizeof(*uses))
				  ? kIOReturnBadArgument
				  : kIOReturnSuccess;
	uint32_t count = 0;
	IOLockLock(lock_);
	if (submitGate_.closed())
		result = kIOReturnNotOpen;
	for (uint32_t i = 0; !result && i < input->use_count; ++i) {
		const auto &use = uses[i];
		constexpr uint32_t mask = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
		const bool ranged = use.access & TegraGM20BSurfaceRange;
		if (!(use.access & mask) ||
		    (ranged ? !(use.access & TegraGM20BSurfaceGpuAddress) || !use.reserved
			    : use.reserved != 0) ||
		    (use.access & ~(mask | TegraGM20BSurfaceGpuAddress |
				    TegraGM20BSurfaceHostWritten | TegraGM20BSurfaceRange))) {
			result = kIOReturnBadArgument;
			break;
		}
		const bool addressed = use.access & TegraGM20BSurfaceGpuAddress;
		const Binding *binding =
			addressed ? switch_gpu_binding_find(binds_, nbinds_, use.handle,
							    ranged ? use.reserved : 1)
				  : nullptr;
		const Bo *bo = addressed ? (binding ? binding->bo : nullptr) : findBo(use.handle);
		uint64_t offset = 0;
		if (!bo || !bo->backing ||
		    (ranged &&
		     !switch_gpu_binding_offset(*binding, use.handle, use.reserved, offset)) ||
		    ((use.access & TegraGM20BSurfaceWrite) && bo->readOnly)) {
			result = kIOReturnBadArgument;
			break;
		}
		if (bo->nativeView) {
			resolved[count++] = {bo->nativeView->logical, nullptr, nullptr, 0,
					     SWITCH_GPU_BACKING_READ};
			resolved[count++] = {bo->nativeView->native, nullptr, nullptr, 0,
					     SWITCH_GPU_BACKING_READ};
		} else {
			resolved[count++] = {bo->backing, bo->surface, bo->representation,
					     bo->representationValue, use.access & mask};
		}
	}
	if (!result) {
		notification->owner = this;
		reference.take(notification->reference);
		retain();
		result = ioreturn(resources_.observe(resolved, count, input->cookie, eventNotified,
						     notification));
		if (result)
			release();
	}
	IOLockUnlock(lock_);
	if (result)
		IODelete(notification, EventNotification, 1);
	IODelete(uses, TegraGM20BAbiSurfaceUse, input->use_count);
	IODelete(resolved, SwitchGpuResourceUse, input->use_count * 2);
	return result;
}
