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

void replaceThread(thread_t &slot, thread_t target)
{
	if (slot == target)
		return;
	thread_t old = slot;
	switch_user_turnstile_thread_retain(target);
	slot = target;
	switch_user_turnstile_thread_drop(old);
}

void setInheritor(Request *request, thread_t target)
{
	if (request->inheritor == target)
		return;
	thread_t old = request->inheritor;
	switch_user_turnstile_thread_retain(target);
	request->inheritor = target;
	// Retarget before dropping `old`: a sleeping waiter that unlocked gBroker.lock after
	// grabbing the turnstile interlock still needs `old` alive until it releases that interlock.
	switch_user_turnstile_retarget(&request->turnstile, target);
	switch_user_turnstile_thread_drop(old);
}

void freeRequest(Request *request)
{
	if (request->async)
		IOUserClient::releaseAsyncReference64(request->reference);
	IODelete(request, Request, 1);
}

// lock held: `request` finished with `result`. Always unlinks `request`, wakes any sleeping
// waiters, and releases its inheritor. An active async request stays alive until send() delivers
// it and every CompileWait waiter has exited; any other request frees once waiters reaches 0.
void finish(Request *request, int32_t result, Request **sends, uint32_t how)
{
	request->done = true;
	request->result = result;
	request->how = how;
	dropBytes(request);
	thread_t old = request->inheritor;
	request->inheritor = nullptr;
	switch_user_turnstile_wakeup_all(&request->turnstile, THREAD_AWAKENED);
	switch_user_turnstile_thread_drop(old);
	unlink(request);
	if (request->async && !request->abandoned) {
		request->sendPending = true;
		request->next = *sends;
		*sends = request;
	} else if (request->waiters == 0) {
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
		const TegraGM20BAbiCompileTimes &t = request->times;
		io_user_reference_t payload[TegraGM20BCompileAnswerWords] = {
			request->cookie,
			uint64_t(uint32_t(request->result)) | uint64_t(generation) << 32,
			t.compile_start,
			t.compile_end,
			t.answered,
			uint64_t(t.answer_priority) | uint64_t(t.reserved) << 32};
		Sender::deliver(request->reference, payload, TegraGM20BCompileAnswerWords);
		bool drop = true;
		if (request->waitedAsync) {
			IOLockLock(gBroker.lock);
			request->sendPending = false;
			drop = request->waiters == 0;
			IOLockUnlock(gBroker.lock);
		}
		if (drop)
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

namespace
{
bool libraryRoom(const void *owner, uint64_t bytes)
{
	uint32_t mine = 0;
	uint64_t mineBytes = 0;
	for (const ClientLibrary *l = gBroker.libraries; l; l = l->next) {
		if (l->owner == owner) {
			mine++;
			mineBytes += l->bytes;
		}
	}
	return mine < TegraGM20BCompilerLibrariesPerClient &&
	       mineBytes + bytes <= TegraGM20BCompilerLibraryBytesPerClient &&
	       gBroker.libraryBytesTotal + bytes <= TegraGM20BCompilerLibraryBytesTotal;
}

CompilerLibraryMap *findLibraryMap(uint64_t client, uint64_t id)
{
	for (CompilerLibraryMap *m = gBroker.libraryMaps; m; m = m->next)
		if (m->client == client && m->id == id)
			return m;
	return nullptr;
}
}  // namespace

IOReturn registerLibrary(const void *owner, task_t task,
			 const TegraGM20BAbiCompilerRegisterLibrary *in,
			 TegraGM20BAbiCompilerRegisterLibraryReply *out)
{
	if (!in->address || !in->bytes || in->bytes > TegraGM20BCompileRequestLimit || in->flags ||
	    in->reserved)
		return kIOReturnBadArgument;
	IOLockLock(gBroker.lock);
	const bool room = libraryRoom(owner, in->bytes);
	IOLockUnlock(gBroker.lock);
	if (!room)
		return kIOReturnNoResources;
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
		in->address, in->bytes,
		IOOptionBits(kIODirectionOut) | IOOptionBits(kIOMemoryMapCopyOnWrite) |
			IOOptionBits(kIOMemoryPersistent),
		task);
	if (!memory)
		return kIOReturnVMError;
	auto *entry = IONew(ClientLibrary, 1);
	if (!entry) {
		memory->release();
		return kIOReturnNoMemory;
	}
	IOLockLock(gBroker.lock);
	const uint64_t cid = libraryRoom(owner, in->bytes) ? clientId(owner) : 0;
	if (!cid) {
		IOLockUnlock(gBroker.lock);
		IODelete(entry, ClientLibrary, 1);
		memory->release();
		return kIOReturnNoResources;
	}
	*entry = {gBroker.libraries, owner, cid, ++gBroker.nextLibraryId, in->bytes, memory};
	gBroker.libraries = entry;
	gBroker.libraryBytesTotal += in->bytes;
	*out = {entry->id};
	IOLockUnlock(gBroker.lock);
	return kIOReturnSuccess;
}

IOReturn mapLibrary(const void *owner, task_t task, const TegraGM20BAbiCompilerMapLibrary *in,
		    TegraGM20BAbiCompilerMapLibraryReply *out)
{
	if (!in->id || !in->client || in->flags || in->reserved)
		return kIOReturnBadArgument;
	IOMemoryDescriptor *memory = nullptr;
	uint64_t bytes = 0;
	IOLockLock(gBroker.lock);
	if (gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		return kIOReturnNotPermitted;
	}
	if (CompilerLibraryMap *existing = findLibraryMap(in->client, in->id)) {
		*out = {uint64_t(existing->map->getAddress()), existing->bytes};
		IOLockUnlock(gBroker.lock);
		return kIOReturnSuccess;
	}
	for (ClientLibrary *l = gBroker.libraries; l; l = l->next) {
		if (l->client == in->client && l->id == in->id) {
			memory = l->memory;
			bytes = l->bytes;
			memory->retain();
			break;
		}
	}
	IOLockUnlock(gBroker.lock);
	if (!memory)
		return kIOReturnNotFound;
	IOMemoryMap *map = memory->createMappingInTask(
		task, 0, IOOptionBits(kIOMapAnywhere) | IOOptionBits(kIOMapReadOnly));
	memory->release();
	if (!map)
		return kIOReturnVMError;
	auto *node = IONew(CompilerLibraryMap, 1);
	if (!node) {
		map->release();
		return kIOReturnNoMemory;
	}
	IOLockLock(gBroker.lock);
	if (gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		IODelete(node, CompilerLibraryMap, 1);
		map->release();
		return kIOReturnNotPermitted;
	}
	if (CompilerLibraryMap *existing = findLibraryMap(in->client, in->id)) {
		*out = {uint64_t(existing->map->getAddress()), existing->bytes};
		IOLockUnlock(gBroker.lock);
		IODelete(node, CompilerLibraryMap, 1);
		map->release();
		return kIOReturnSuccess;
	}
	*node = {gBroker.libraryMaps, in->client, in->id, bytes, map};
	gBroker.libraryMaps = node;
	*out = {uint64_t(map->getAddress()), bytes};
	IOLockUnlock(gBroker.lock);
	return kIOReturnSuccess;
}

IOReturn unmapLibrary(const void *owner, const TegraGM20BAbiCompilerUnmapLibrary *in)
{
	if (!in->id || !in->client || in->flags || in->reserved)
		return kIOReturnBadArgument;
	CompilerLibraryMap *found = nullptr;
	IOLockLock(gBroker.lock);
	if (gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		return kIOReturnNotPermitted;
	}
	for (CompilerLibraryMap **p = &gBroker.libraryMaps; *p; p = &(*p)->next) {
		if ((*p)->client == in->client && (*p)->id == in->id) {
			found = *p;
			*p = found->next;
			break;
		}
	}
	IOLockUnlock(gBroker.lock);
	if (!found)
		return kIOReturnNotFound;
	found->map->release();
	IODelete(found, CompilerLibraryMap, 1);
	return kIOReturnSuccess;
}

ClientLibrary *takeLibraries(const void *owner)
{
	ClientLibrary *dropped = nullptr;
	for (ClientLibrary **p = &gBroker.libraries; *p;) {
		if ((*p)->owner == owner) {
			ClientLibrary *l = *p;
			*p = l->next;
			gBroker.libraryBytesTotal -= l->bytes;
			l->next = dropped;
			dropped = l;
		} else {
			p = &(*p)->next;
		}
	}
	return dropped;
}

CompilerLibraryMap *takeLibraryMaps()
{
	CompilerLibraryMap *maps = gBroker.libraryMaps;
	gBroker.libraryMaps = nullptr;
	return maps;
}

void dropLibraries(ClientLibrary *list)
{
	while (ClientLibrary *l = list) {
		list = l->next;
		l->memory->release();
		IODelete(l, ClientLibrary, 1);
	}
}

void dropLibraryMaps(CompilerLibraryMap *list)
{
	while (CompilerLibraryMap *m = list) {
		list = m->next;
		m->map->release();
		IODelete(m, CompilerLibraryMap, 1);
	}
}

}  // namespace switch_gpu_compiler
