// SPDX-License-Identifier: MIT
// See SwitchGpuShaderCompiler.h and TegraGM20BCompilerABI.h; the requests' life is SwitchGpuShaderCompilerQueue.h's.
#include "SwitchGpuShaderCompiler.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include "SwitchGpuShaderCompilerPolicy.h"
#include "SwitchGpuShaderCompilerQueue.h"
#include "TegraGM20BCompilerABI.h"

extern "C" int proc_selfpid(void);

namespace
{
using namespace switch_gpu_compiler;

constexpr uint64_t kStoreLimit = 1ULL << 30, kPageMask = 0x3fff;
// A request the compiler held when it went is given to the next one; one held by two that went is not tried again
constexpr uint32_t kAttempts = 2;

// lock held. iOS 17 has no launcher for the compiler but the BlueTool stand-in, which switch-no-bt hides: with it set, a
// request finds the compiler registered or none at all, and its process compiles the state itself. The same once a
// compiler declined, or went with nothing to start it again.
bool compilerMayCome()
{
	static int noBluetooth = -1;
	if (noBluetooth < 0) {
		uint32_t off = 0;
		noBluetooth = PE_parse_boot_argn("switch-no-bt", &off, sizeof(off)) && off;
	}
	return !noBluetooth && !gBroker.declined;
}

IOReturn info(TegraGM20BAbiCompilerInfoReply *out)
{
	IOLockLock(gBroker.lock);
	*out = {gBroker.store ? gBroker.storeBytes : 0, gBroker.stack,
		TegraGM20BCompilerCapable |
			(gBroker.compiler ? uint32_t(TegraGM20BCompilerRegistered) : 0u) |
			(gBroker.store ? uint32_t(TegraGM20BCompilerStore) : 0u) |
			(compilerMayCome() ? uint32_t(TegraGM20BCompilerExpected) : 0u) |
			(gBroker.declined ? uint32_t(TegraGM20BCompilerDeclined) : 0u),
		gBroker.generation};
	IOLockUnlock(gBroker.lock);
	return kIOReturnSuccess;
}

IOReturn waitFor(Request *request, TegraGM20BAbiCompileReply *out)
{
	while (!request->done)
		if (IOLockSleep(gBroker.lock, request, THREAD_ABORTSAFE) == THREAD_INTERRUPTED)
			break;
	if (request->done) {
		*out = {request->result, gBroker.generation, request->how, 0};
		unlink(request);
		IOLockUnlock(gBroker.lock);
		freeRequest(request);
		return kIOReturnSuccess;
	}
	request->waiter = false;
	if (!request->taken) {
		dropBytes(request);
		unlink(request);
		freeRequest(request);
	}
	IOLockUnlock(gBroker.lock);
	return kIOReturnAborted;
}

IOReturn compile(const void *owner, task_t task, const TegraGM20BAbiCompile *in, void *out,
		 uint32_t outSize, const IOExternalMethodArguments *arguments,
		 SwitchGpuAsyncReference &reference)
{
	const bool async = arguments->asyncReference && arguments->asyncReferenceCount;
	if (!in->address || !in->bytes || in->bytes > TegraGM20BCompileRequestLimit ||
	    (in->flags & ~uint32_t(TegraGM20BCompileWaited)))
		return kIOReturnBadArgument;
	if (async ? (outSize || arguments->asyncReferenceCount < 3 || !arguments->asyncWakePort)
		  : outSize != sizeof(TegraGM20BAbiCompileReply))
		return kIOReturnBadArgument;
	auto *reply = static_cast<TegraGM20BAbiCompileReply *>(out);
	IOLockLock(gBroker.lock);
	if (gBroker.compiler ? gBroker.stack != in->stack : !compilerMayCome()) {
		const uint32_t generation = gBroker.generation;
		IOLockUnlock(gBroker.lock);
		if (async)
			return kIOReturnNotReady;
		*reply = {TegraGM20BCompileUnavailable, generation, 0, 0};
		return kIOReturnSuccess;
	}
	const bool room = reserve(owner, in->bytes, async);
	IOLockUnlock(gBroker.lock);
	if (!room)
		return kIOReturnNoResources;
	IOBufferMemoryDescriptor *bytes = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPageable),
		in->bytes);
	if (!bytes || !copyTask(task, in->address, in->bytes, bytes, true)) {
		OSSafeReleaseNULL(bytes);
		unreserve(in->bytes);
		return bytes ? kIOReturnVMError : kIOReturnNoMemory;
	}
	auto *request = IONew(Request, 1);
	if (!request) {
		bytes->release();
		unreserve(in->bytes);
		return kIOReturnNoMemory;
	}
	*request = {};
	request->owner = owner;
	request->bytes = bytes;
	request->size = in->bytes;
	request->cookie = in->cookie;
	request->qos = in->qos;
	request->flags = in->flags;
	request->pid = proc_selfpid();
	request->async = async;
	request->waiter = !async;
	IOLockLock(gBroker.lock);
	request->id = ++gBroker.nextId;
	request->client = clientId(owner);
	Request **tail = &gBroker.requests;
	while (*tail)
		tail = &(*tail)->next;
	*tail = request;
	IOLockWakeup(gBroker.lock, &gBroker.requests, true);
	if (async) {
		reference.take(request->reference);
		IOLockUnlock(gBroker.lock);
		return kIOReturnSuccess;
	}
	return waitFor(request, reply);
}

IOReturn registerCompiler(const void *owner, task_t task, const TegraGM20BAbiCompilerRegister *in,
			  TegraGM20BAbiCompilerRegisterReply *out)
{
	if (!entitled(task))
		return kIOReturnNotPrivileged;
	if (!in->store_address || !in->store_bytes || in->store_bytes > kStoreLimit || !in->stack ||
	    ((in->store_address | in->store_bytes) & kPageMask) ||
	    (in->flags & ~uint32_t(TegraGM20BCompilerRestarts)) || in->reserved)
		return kIOReturnBadArgument;
	IOMemoryDescriptor *store = IOMemoryDescriptor::withAddressRange(
		in->store_address, in->store_bytes,
		IOOptionBits(kIODirectionOut) | IOOptionBits(kIOMemoryPersistent), task);
	if (!store)
		return kIOReturnVMError;
	IOLockLock(gBroker.lock);
	if (gBroker.compiler && gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		store->release();
		return kIOReturnExclusiveAccess;
	}
	IOMemoryDescriptor *old = gBroker.store;
	gBroker.compiler = owner;
#if SWITCH_GPU_COMPILER_LINE
	gBroker.compilerPid = proc_selfpid();
#endif
	gBroker.announced = nullptr;
	gBroker.store = store;
	gBroker.storeBytes = in->store_bytes;
	gBroker.stack = in->stack;
	gBroker.restarts = in->flags & TegraGM20BCompilerRestarts;
	gBroker.declined = false;
	registeredAfterLosses(gBroker.lossesInARow);
	*out = {++gBroker.generation, 0};
	const uint32_t generation = gBroker.generation;
	IOLockUnlock(gBroker.lock);
	OSSafeReleaseNULL(old);
	IOLog("SwitchShaderCompiler: pid %d registered, store %llu MiB, stack %016llx, generation "
	      "%u\n",
	      proc_selfpid(), (unsigned long long)(in->store_bytes >> 20),
	      (unsigned long long)in->stack, generation);
	return kIOReturnSuccess;
}

IOReturn take(const void *owner, task_t task, const TegraGM20BAbiCompilerTake *in,
	      TegraGM20BAbiCompilerTakeReply *out)
{
	IOLockLock(gBroker.lock);
	Request *request = nullptr;
	while (gBroker.compiler == owner) {
		for (request = gBroker.requests; request && request->taken;
		     request = request->next) {
		}
		if (request)
			break;
		if (IOLockSleep(gBroker.lock, &gBroker.requests, THREAD_ABORTSAFE) ==
		    THREAD_INTERRUPTED)
			break;
	}
	if (!request || gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		return gBroker.compiler == owner ? kIOReturnAborted : kIOReturnNotPermitted;
	}
	*out = {request->id,
		request->size,
		request->client,
		request->qos,
		request->flags,
		request->pid,
		0};
	if (request->size > in->buffer_bytes) {
		out->id = 0;
		IOLockUnlock(gBroker.lock);
		return kIOReturnSuccess;
	}
	request->taken = request->copying = true;
	IOBufferMemoryDescriptor *bytes = request->bytes;
	bytes->retain();
	IOLockUnlock(gBroker.lock);
	const bool copied = copyTask(task, in->buffer_address, request->size, bytes, false);
	bytes->release();
	Request *sends = nullptr;
	IOLockLock(gBroker.lock);
	request->copying = false;
	IOReturn ret = kIOReturnSuccess;
	if (gBroker.compiler != owner) {
		if (gBroker.declined)
			finish(request, TegraGM20BCompileUnavailable, &sends);
		else
			request->taken = false;
		ret = kIOReturnNotPermitted;
	} else if (!copied) {
		finish(request, TegraGM20BCompileFailed, &sends);
		ret = kIOReturnVMError;
	}
	const uint32_t generation = gBroker.generation;
	IOLockUnlock(gBroker.lock);
	send(sends, generation);
	return ret;
}

IOReturn completeRequest(const void *owner, const TegraGM20BAbiCompilerComplete *in)
{
	if (in->result < TegraGM20BCompileMade || in->result > TegraGM20BCompileUnavailable ||
	    (in->how & 0xff) > TegraGM20BCompileStored || (in->how >> 16))
		return kIOReturnBadArgument;
	Request *sends = nullptr;
	IOLockLock(gBroker.lock);
	if (gBroker.compiler != owner) {
		IOLockUnlock(gBroker.lock);
		return kIOReturnNotPermitted;
	}
	Request *request = gBroker.requests;
	while (request &&
	       (request->id != in->id || !request->taken || request->copying || request->done))
		request = request->next;
	if (request)
		finish(request, in->result, &sends, in->how);
	const uint32_t generation = gBroker.generation;
	IOLockUnlock(gBroker.lock);
	send(sends, generation);
	return request ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn announce(const void *owner, task_t task, const TegraGM20BAbiCompilerAnnounce *in)
{
	if (!entitled(task))
		return kIOReturnNotPrivileged;
	if ((in->flags & ~uint32_t(TegraGM20BCompilerRestarts)) || in->reserved)
		return kIOReturnBadArgument;
	IOLockLock(gBroker.lock);
	if (gBroker.compiler != owner) {
		gBroker.announced = owner;
		gBroker.restarts = in->flags & TegraGM20BCompilerRestarts;
	}
	IOLockUnlock(gBroker.lock);
	return kIOReturnSuccess;
}

IOReturn decline(task_t task)
{
	if (!entitled(task))
		return kIOReturnNotPrivileged;
	Request *sends = nullptr;
	IOLockLock(gBroker.lock);
	gBroker.declined = true;
	const uint32_t ended =
		gBroker.compiler ? 0 : finishAll(TegraGM20BCompileUnavailable, &sends);
	const uint32_t generation = gBroker.generation;
	IOLockUnlock(gBroker.lock);
	send(sends, generation);
	IOLog("SwitchShaderCompiler: pid %d declined to serve; %u waiting requests unavailable, "
	      "their processes compile them\n",
	      proc_selfpid(), ended);
	return kIOReturnSuccess;
}

}  // namespace

bool switch_gpu_compiler_method(uint32_t method)
{
	return method >= TegraGM20BMethodCompilerInfo && method <= TegraGM20BMethodCompilerAnnounce;
}

IOReturn switch_gpu_compiler_call(const void *owner, task_t task, uint32_t method, const void *in,
				  uint32_t inSize, void *out, uint32_t outSize,
				  const IOExternalMethodArguments *arguments,
				  SwitchGpuAsyncReference &reference)
{
	if (!brokerLock())
		return kIOReturnNoMemory;
	const bool async = arguments->asyncReference && arguments->asyncReferenceCount;
	if (async && method != TegraGM20BMethodCompile)
		return kIOReturnBadArgument;
	switch (method) {
	case TegraGM20BMethodCompilerInfo:
		if (inSize || outSize != sizeof(TegraGM20BAbiCompilerInfoReply))
			return kIOReturnBadArgument;
		return info(static_cast<TegraGM20BAbiCompilerInfoReply *>(out));
	case TegraGM20BMethodCompile:
		if (inSize != sizeof(TegraGM20BAbiCompile))
			return kIOReturnBadArgument;
		return compile(owner, task, static_cast<const TegraGM20BAbiCompile *>(in), out,
			       outSize, arguments, reference);
	case TegraGM20BMethodCompilerRegister:
		if (inSize != sizeof(TegraGM20BAbiCompilerRegister) ||
		    outSize != sizeof(TegraGM20BAbiCompilerRegisterReply))
			return kIOReturnBadArgument;
		return registerCompiler(owner, task,
					static_cast<const TegraGM20BAbiCompilerRegister *>(in),
					static_cast<TegraGM20BAbiCompilerRegisterReply *>(out));
	case TegraGM20BMethodCompilerTake:
		if (inSize != sizeof(TegraGM20BAbiCompilerTake) ||
		    outSize != sizeof(TegraGM20BAbiCompilerTakeReply))
			return kIOReturnBadArgument;
		return take(owner, task, static_cast<const TegraGM20BAbiCompilerTake *>(in),
			    static_cast<TegraGM20BAbiCompilerTakeReply *>(out));
	case TegraGM20BMethodCompilerComplete:
		if (inSize != sizeof(TegraGM20BAbiCompilerComplete) || outSize)
			return kIOReturnBadArgument;
		return completeRequest(owner,
				       static_cast<const TegraGM20BAbiCompilerComplete *>(in));
	case TegraGM20BMethodCompilerDecline:
		if (inSize || outSize)
			return kIOReturnBadArgument;
		return decline(task);
	case TegraGM20BMethodCompilerAnnounce:
		if (inSize != sizeof(TegraGM20BAbiCompilerAnnounce) || outSize)
			return kIOReturnBadArgument;
		return announce(owner, task,
				static_cast<const TegraGM20BAbiCompilerAnnounce *>(in));
	default:
		return kIOReturnUnsupported;
	}
}

IOReturn switch_gpu_compiler_store(IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (!brokerLock())
		return kIOReturnNoMemory;
	IOLockLock(gBroker.lock);
	IOMemoryDescriptor *store = gBroker.store;
	if (store)
		store->retain();
	IOLockUnlock(gBroker.lock);
	if (!store)
		return kIOReturnBadArgument;
	*memory = store;
	*options = kIOMapAnywhere | kIOMapReadOnly;
	return kIOReturnSuccess;
}

#if SWITCH_GPU_COMPILER_LINE
void switch_gpu_compiler_line(char *line, size_t size)
{
	if (!size)
		return;
	line[0] = 0;
	if (!brokerLock())
		return;
	IOLockLock(gBroker.lock);
	const int32_t pid = gBroker.compilerPid;
	const uint32_t generation = gBroker.generation, losses = gBroker.losses;
	const bool declined = gBroker.declined;
	IOLockUnlock(gBroker.lock);
	snprintf(line, size, "pid=%d generation=%u losses=%u declined=%d", pid, generation, losses,
		 declined ? 1 : 0);
}
#endif

void switch_gpu_compiler_close(const void *owner)
{
	if (!__atomic_load_n(&gBroker.lock, __ATOMIC_ACQUIRE))
		return;
	Request *sends = nullptr;
	uint32_t ended = 0, requeued = 0;
	IOLockLock(gBroker.lock);
	const bool compiler = gBroker.compiler == owner;
	const bool announced = !compiler && gBroker.announced == owner;
	if (compiler) {
		gBroker.compiler = nullptr;
#if SWITCH_GPU_COMPILER_LINE
		gBroker.compilerPid = 0;
#endif
	}
	if (announced)
		gBroker.announced = nullptr;
	// one that announced itself while another compiler serves (a second instance) ends nothing
	const bool lost = compiler || (announced && !gBroker.compiler);
	if (lost) {
		gBroker.losses++;
		gBroker.declined = declinedAfterClose(gBroker.declined, gBroker.restarts,
						      gBroker.lossesInARow);
	}
	if (lost && gBroker.declined)
		ended = finishAll(TegraGM20BCompileUnavailable, &sends);
	for (Request *request = gBroker.requests, *next; request; request = next) {
		next = request->next;
		if (compiler && request->taken && !request->copying && !request->done) {
			if (++request->attempts >= kAttempts) {
				finish(request, TegraGM20BCompileFailed, &sends);
				ended++;
			} else {
				request->taken = false;
				requeued++;
			}
		} else if (request->owner == owner && request->async) {
			request->abandoned = true;
			if (!request->taken) {
				dropBytes(request);
				unlink(request);
				freeRequest(request);
			}
		}
	}
	for (Client &c : gBroker.clients)
		if (c.owner == owner)
			c = {};
	if (compiler)
		IOLockWakeup(gBroker.lock, &gBroker.requests, false);
	const uint32_t generation = gBroker.generation;
	const bool declined = gBroker.declined;
	IOLockUnlock(gBroker.lock);
	send(sends, generation);
	if (lost)
		IOLog("SwitchShaderCompiler: the compiler's connection closed; "
		      "%u requests wait for the next one, %u end %s\n",
		      requeued, ended,
		      declined ? "unavailable (no compiler comes: their processes compile them)"
			       : "failed (held by two compilers that went)");
}
