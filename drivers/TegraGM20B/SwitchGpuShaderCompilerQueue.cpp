// SPDX-License-Identifier: MIT
// See SwitchGpuShaderCompilerQueue.h.
#include "SwitchGpuShaderCompilerQueue.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSBoolean.h>
#include "TegraGM20BCompilerABI.h"

namespace switch_gpu_compiler
{
namespace
{
constexpr const char kEntitlement[] = "com.brunostjohn.switch.shader-compiler";
}  // namespace

Broker gBroker;

IOLock *brokerLock()
{
	IOLock *lock = __atomic_load_n(&gBroker.lock, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *made = IOLockAlloc();
	IOLock *expected = nullptr;
	if (made && !__atomic_compare_exchange_n(&gBroker.lock, &expected, made, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return expected;
	}
	return made;
}

// lock held: the id the compiler knows `owner` by, made at its first request
uint64_t clientId(const void *owner)
{
	Client *free = nullptr;
	for (Client &c : gBroker.clients) {
		if (c.owner == owner)
			return c.id;
		if (!c.owner && !free)
			free = &c;
	}
	if (!free)
		return 0;
	*free = {owner, ++gBroker.nextClient};
	return free->id;
}

void unlink(Request *request)
{
	for (Request **p = &gBroker.requests; *p; p = &(*p)->next)
		if (*p == request) {
			*p = request->next;
			gBroker.pending--;
			return;
		}
}

void dropBytes(Request *request)
{
	if (!request->bytes)
		return;
	request->bytes->release();
	request->bytes = nullptr;
	gBroker.queuedBytes -= request->size;
}

void freeRequest(Request *request)
{
	if (request->async)
		IOUserClient::releaseAsyncReference64(request->reference);
	IODelete(request, Request, 1);
}

// lock held: `request` finished with `result`. An asynchronous one is unlinked onto `sends` (its result goes out after the
// lock is dropped); a synchronous one's waiter is woken.
void finish(Request *request, int32_t result, Request **sends, uint32_t how)
{
	request->done = true;
	request->result = result;
	request->how = how;
	dropBytes(request);
	if (request->waiter) {
		IOLockWakeup(gBroker.lock, request, false);
	} else if (request->async && !request->abandoned) {
		unlink(request);
		request->next = *sends;
		*sends = request;
	} else {
		unlink(request);
		freeRequest(request);
	}
}

// lock held: every request not finished nor being copied out finished with `result`; how many
uint32_t finishAll(int32_t result, Request **sends)
{
	uint32_t ended = 0;
	for (Request *request = gBroker.requests, *next; request; request = next) {
		next = request->next;
		if (!request->copying && !request->done) {
			finish(request, result, sends);
			ended++;
		}
	}
	return ended;
}

// IOUserClient's result sender, which only a user client may call
class Sender : public IOUserClient
{
public:
	static void deliver(OSAsyncReference64 reference, io_user_reference_t *payload,
			    uint32_t count)
	{
		sendAsyncResult64(reference, kIOReturnSuccess, payload, count);
	}
};

void send(Request *sends, uint32_t generation)
{
	while (Request *request = sends) {
		sends = request->next;
		io_user_reference_t payload[] = {request->cookie,
						 uint64_t(uint32_t(request->result)) |
							 uint64_t(generation) << 32};
		Sender::deliver(request->reference, payload, 2);
		freeRequest(request);
	}
}

bool entitled(task_t task)
{
	OSObject *value = IOUserClient::copyClientEntitlement(task, kEntitlement);
	const bool yes = value == kOSBooleanTrue;
	OSSafeReleaseNULL(value);
	return yes;
}

// [address, + bytes) of `task`'s memory into or out of `buffer`
bool copyTask(task_t task, uint64_t address, uint64_t bytes, IOBufferMemoryDescriptor *buffer,
	      bool intoBuffer)
{
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
		address, bytes, intoBuffer ? kIODirectionOut : kIODirectionIn, task);
	if (!memory)
		return false;
	bool ok = memory->prepare() == kIOReturnSuccess;
	if (ok) {
		void *kernel = buffer->getBytesNoCopy();
		ok = (intoBuffer ? memory->readBytes(0, kernel, bytes)
				 : memory->writeBytes(0, kernel, bytes)) == bytes;
		memory->complete();
	}
	memory->release();
	return ok;
}

// lock held: room for one more of `owner`'s requests of `size` bytes (reserved when there is). A synchronous request is a
// thread asleep on it, so only asynchronous ones count against the connection's bound.
bool reserve(const void *owner, uint64_t size, bool async)
{
	uint32_t mine = 0;
	for (Request *r = gBroker.requests; r; r = r->next)
		mine += r->owner == owner && r->async;
	if ((async && mine >= TegraGM20BCompilePendingPerClient) ||
	    gBroker.pending >= TegraGM20BCompilePending ||
	    gBroker.queuedBytes + size > TegraGM20BCompileQueuedBytes)
		return false;
	gBroker.pending++;
	gBroker.queuedBytes += size;
	return true;
}

void unreserve(uint64_t size)
{
	IOLockLock(gBroker.lock);
	gBroker.pending--;
	gBroker.queuedBytes -= size;
	IOLockUnlock(gBroker.lock);
}

}  // namespace switch_gpu_compiler
