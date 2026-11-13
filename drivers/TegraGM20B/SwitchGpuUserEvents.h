// SPDX-License-Identifier: MIT
#pragma once

void AGXDeviceUserClient::eventNotified(void *context, uint64_t cookie, int result)
{
	auto *notification = static_cast<EventNotification *>(context);
	io_user_reference_t payload[] = {cookie};
	sendAsyncResult64(notification->reference, result ? kIOReturnAborted : kIOReturnSuccess,
			  payload, 1);
	releaseAsyncReference64(notification->reference);
	auto *owner = notification->owner;
	IODelete(notification, EventNotification, 1);
	owner->release();
}

IOReturn AGXDeviceUserClient::eventObserve(const TegraGM20BAbiEventObserve *input,
					   IOExternalMethodArguments *arguments,
					   SwitchGpuAsyncReference &reference)
{
	constexpr uint32_t modes = TegraGM20BEventObserveProbe | TegraGM20BEventObserveTerminal;
	if (!input->handle || input->reserved ||
	    ((input->flags & ~modes) && input->flags != TegraGM20BEventObserveCancel))
		return kIOReturnBadArgument;
	const bool terminal = (input->flags & TegraGM20BEventObserveTerminal) != 0;
	if (input->flags == TegraGM20BEventObserveCancel ||
	    input->flags & TegraGM20BEventObserveProbe) {
		if (arguments->asyncReferenceCount)
			return kIOReturnBadArgument;
		const int result = input->flags == TegraGM20BEventObserveCancel
					   ? events_.cancel(input->handle, input->cookie)
					   : events_.probe(input->handle, input->value, terminal);
		return result == -5 ? kIOReturnAborted : ioreturn(result);
	}
	if (!arguments->asyncReference || arguments->asyncReferenceCount < 3 ||
	    arguments->asyncReferenceCount > kOSAsyncRef64Count || !arguments->asyncWakePort)
		return kIOReturnBadArgument;
	auto *notification = IONew(EventNotification, 1);
	if (!notification)
		return kIOReturnNoMemory;
	notification->owner = this;
	reference.take(notification->reference);
	retain();
	const int result = events_.observe(input->handle, input->value, input->cookie,
					   eventNotified, notification, terminal);
	if (result) {
		IODelete(notification, EventNotification, 1);
		release();
	}
	return result == -5 ? kIOReturnAborted : ioreturn(result);
}
