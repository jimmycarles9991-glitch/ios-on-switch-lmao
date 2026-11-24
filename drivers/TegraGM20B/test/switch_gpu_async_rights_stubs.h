// SPDX-License-Identifier: MIT
// IOKit for the asynchronous calls' send rights (switch_gpu_async_rights_test.cpp, switch_gpu_compiler_rights_test.cpp):
// a port is a number, its send rights a count. A release with none left is the kernel's "send right count overflow"
// panic, counted in `underflows`; a reply on a port with no right is counted in `orphanSends`.
#pragma once
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mach/port.h>
#include <map>
#include <mutex>
#include <vector>

using IOReturn = int;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using UInt32 = uint32_t;
using io_user_reference_t = uint64_t;
using mach_vm_address_t = uint64_t;
using task_t = struct host_task *;
using thread_t = struct host_thread *;

struct host_thread {
	const char *name;
};

constexpr thread_t THREAD_NULL = nullptr;

constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 0x2bc, kIOReturnNoMemory = 0x2bd,
		   kIOReturnNoResources = 0x2be, kIOReturnBadArgument = 0x2c2,
		   kIOReturnExclusiveAccess = 0x2c5, kIOReturnUnsupported = 0x2c7,
		   kIOReturnVMError = 0x2c8, kIOReturnNotPrivileged = 0x2c1,
		   kIOReturnNotPermitted = 0x2e2, kIOReturnBusy = 0x2d5, kIOReturnTimeout = 0x2d6,
		   kIOReturnNotReady = 0x2d8, kIOReturnNotResponding = 0x2ed,
		   kIOReturnAborted = 0x2eb, kIOReturnNotOpen = 0x2cd, kIOReturnNotFound = 0x2f0;
constexpr uint32_t kOSAsyncRef64Count = 8;
using OSAsyncReference64 = io_user_reference_t[kOSAsyncRef64Count];
constexpr io_user_reference_t kIOUCAsync64Flag = 1, kIOUCAsync0Flags = 3;

enum {
	kIOAsyncReservedIndex = 0,
	kIOAsyncCalloutFuncIndex = 1,
	kIOAsyncCalloutRefconIndex = 2
};

enum : IOOptionBits {
	kIODirectionIn = 1,
	kIODirectionOut = 2,
	kIODirectionInOut = 3,
	kIOMemoryPageable = 1u << 10,
	kIOMemoryPersistent = 1u << 11,
	kIOMapAnywhere = 1,
	kIOMapReadOnly = 1u << 12,
};

#define APPLE_KEXT_OVERRIDE override

struct HostRights {
	std::map<mach_port_t, int> rights;

	struct Reply {
		mach_port_t port;
		IOReturn result;
		std::vector<io_user_reference_t> arguments;
	};

	std::vector<Reply> replies;
	unsigned underflows = 0, orphanSends = 0;

	void make(mach_port_t port)
	{
		rights[port]++;
	}

	void release(mach_port_t port)
	{
		if (rights[port] <= 0)
			underflows++;
		else
			rights[port]--;
	}

	int held() const
	{
		int sum = 0;
		for (const auto &[port, count] : rights)
			sum += count;
		return sum;
	}

	void reset()
	{
		*this = {};
	}
};

inline HostRights gRights;

class OSObject
{
public:
	virtual ~OSObject() = default;

	void retain()
	{
		references_++;
	}

	void release()
	{
		if (--references_ == 0)
			delete this;
	}

private:
	int references_ = 1;
};

class OSBoolean : public OSObject
{
};

inline OSBoolean *const kOSBooleanTrue = new OSBoolean;
#define OSSafeReleaseNULL(object)            \
	do {                                 \
		if (object)                  \
			(object)->release(); \
		(object) = nullptr;          \
	} while (0)

struct host_task {
	bool entitled;
};

inline host_task gKernelTask = {false};
inline task_t kernel_task = &gKernelTask;

class IOMemoryDescriptor : public OSObject
{
public:
	static IOMemoryDescriptor *withAddressRange(mach_vm_address_t address, uint64_t bytes,
						    IOOptionBits, task_t)
	{
		auto *made = new IOMemoryDescriptor;
		made->address_ = address;
		made->bytes_ = bytes;
		return made;
	}

	IOReturn prepare()
	{
		return kIOReturnSuccess;
	}

	void complete()
	{
	}

	uint64_t readBytes(uint64_t offset, void *out, uint64_t bytes)
	{
		memcpy(out, reinterpret_cast<const uint8_t *>(address_) + offset, bytes);
		return bytes;
	}

	uint64_t writeBytes(uint64_t offset, const void *in, uint64_t bytes)
	{
		memcpy(reinterpret_cast<uint8_t *>(address_) + offset, in, bytes);
		return bytes;
	}

private:
	mach_vm_address_t address_ = 0;
	uint64_t bytes_ = 0;
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor
{
public:
	static IOBufferMemoryDescriptor *inTaskWithOptions(task_t, IOOptionBits, uint64_t bytes)
	{
		auto *made = new IOBufferMemoryDescriptor;
		made->storage_.resize(bytes);
		return made;
	}

	void *getBytesNoCopy()
	{
		return storage_.data();
	}

private:
	std::vector<uint8_t> storage_;
};

struct IOExternalMethodArguments {
	io_user_reference_t *asyncReference;
	uint32_t asyncReferenceCount;
	mach_port_t asyncWakePort;
	const void *structureInput;
	uint32_t structureInputSize;
	void *structureOutput;
	uint32_t structureOutputSize;
	IOMemoryDescriptor *structureInputDescriptor, *structureOutputDescriptor;
	uint32_t scalarInputCount, scalarOutputCount;
};

class IOUserClient : public OSObject
{
public:
	static IOReturn releaseAsyncReference64(OSAsyncReference64 reference)
	{
		const mach_port_t port = mach_port_t(reference[0] & ~kIOUCAsync0Flags);
		if (port)
			gRights.release(port);
		return kIOReturnSuccess;
	}

	static IOReturn releaseNotificationPort(mach_port_t port)
	{
		if (port)
			gRights.release(port);
		return kIOReturnSuccess;
	}

	static OSObject *copyClientEntitlement(task_t task, const char *)
	{
		if (!task->entitled)
			return nullptr;
		kOSBooleanTrue->retain();
		return kOSBooleanTrue;
	}

protected:
	static IOReturn sendAsyncResult64(OSAsyncReference64 reference, IOReturn result,
					  io_user_reference_t *arguments, uint32_t count)
	{
		const mach_port_t port = mach_port_t(reference[0] & ~kIOUCAsync0Flags);
		if (gRights.rights[port] <= 0)
			gRights.orphanSends++;
		gRights.replies.push_back({port, result, {arguments, arguments + count}});
		return kIOReturnSuccess;
	}
};

using IOLock = std::mutex;
using lck_mtx_t = std::mutex;

inline lck_mtx_t *IOLockGetMachLock(IOLock *lock)
{
	return lock;
}

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

inline void __attribute__((no_thread_safety_analysis)) IOLockLock(IOLock *lock)
{
	lock->lock();
}

inline void __attribute__((no_thread_safety_analysis)) IOLockUnlock(IOLock *lock)
{
	lock->unlock();
}

constexpr int THREAD_ABORTSAFE = 1, THREAD_AWAKENED = 0, THREAD_INTERRUPTED = 2;

struct turnstile;

struct switch_user_turnstile {
	struct turnstile *turnstile;
};

inline host_thread gDefaultThread = {"default"};
inline thread_local thread_t gCurrentThread = &gDefaultThread;

inline thread_t current_thread(void)
{
	return gCurrentThread;
}

struct HostTurnstiles {
	struct Slot {
		thread_t inheritor = THREAD_NULL;
		uint32_t waiters = 0;
		uint64_t wakes = 0;
		bool active = false;
	};

	struct Event {
		enum Kind {
			Sleep,
			Retarget,
			WakeAll
		};

		Kind kind;
		const switch_user_turnstile *ut;
		thread_t inheritor;
	};

	std::mutex lock;
	std::condition_variable cv;
	std::map<thread_t, int> refs;
	std::map<const switch_user_turnstile *, Slot> slots;
	std::vector<Event> events;
	unsigned underflows = 0;
	bool abortSleep = false;
	std::function<void(const switch_user_turnstile *, thread_t)> onSleep;

	void retain(thread_t t)
	{
		if (!t)
			return;
		std::lock_guard<std::mutex> l(lock);
		refs[t]++;
	}

	void drop(thread_t t)
	{
		if (!t)
			return;
		std::lock_guard<std::mutex> l(lock);
		if (refs[t] <= 0)
			underflows++;
		else
			refs[t]--;
	}

	int held()
	{
		std::lock_guard<std::mutex> l(lock);
		int sum = 0;
		for (const auto &[t, count] : refs)
			sum += count;
		return sum;
	}

	int refCount(thread_t t)
	{
		std::lock_guard<std::mutex> l(lock);
		auto it = refs.find(t);
		return it == refs.end() ? 0 : it->second;
	}

	void reset()
	{
		std::lock_guard<std::mutex> l(lock);
		refs.clear();
		slots.clear();
		events.clear();
		underflows = 0;
		abortSleep = false;
		onSleep = nullptr;
	}
};

inline HostTurnstiles gTurnstiles;

inline int __attribute__((no_thread_safety_analysis))
switch_user_turnstile_sleep(lck_mtx_t *mtx, struct switch_user_turnstile *ut, thread_t inheritor,
			    int)
{
	std::function<void(const switch_user_turnstile *, thread_t)> hook;
	uint64_t seenWakes = 0;
	{
		std::unique_lock<std::mutex> tl(gTurnstiles.lock);
		mtx->unlock();
		auto &slot = gTurnstiles.slots[ut];
		slot.waiters++;
		slot.active = true;
		slot.inheritor = inheritor;
		ut->turnstile = reinterpret_cast<struct turnstile *>(uintptr_t(1));
		seenWakes = slot.wakes;
		gTurnstiles.events.push_back({HostTurnstiles::Event::Sleep, ut, inheritor});
		hook = gTurnstiles.onSleep;
		gTurnstiles.cv.notify_all();
	}
	if (hook)
		hook(ut, inheritor);
	int wr = THREAD_AWAKENED;
	{
		std::unique_lock<std::mutex> tl(gTurnstiles.lock);
		auto &slot = gTurnstiles.slots[ut];
		if (gTurnstiles.abortSleep) {
			wr = THREAD_INTERRUPTED;
		} else {
			gTurnstiles.cv.wait(tl, [&] {
				return slot.wakes != seenWakes || gTurnstiles.abortSleep;
			});
			if (slot.wakes == seenWakes && gTurnstiles.abortSleep)
				wr = THREAD_INTERRUPTED;
		}
		if (--slot.waiters == 0) {
			slot.active = false;
			slot.inheritor = THREAD_NULL;
			ut->turnstile = nullptr;
		}
		gTurnstiles.cv.notify_all();
	}
	mtx->lock();
	return wr;
}

inline void switch_user_turnstile_retarget(struct switch_user_turnstile *ut, thread_t inheritor)
{
	std::lock_guard<std::mutex> tl(gTurnstiles.lock);
	auto it = gTurnstiles.slots.find(ut);
	if (it == gTurnstiles.slots.end() || !it->second.active)
		return;
	it->second.inheritor = inheritor;
	gTurnstiles.events.push_back({HostTurnstiles::Event::Retarget, ut, inheritor});
	gTurnstiles.cv.notify_all();
}

inline void switch_user_turnstile_wakeup_all(struct switch_user_turnstile *ut, int)
{
	std::lock_guard<std::mutex> tl(gTurnstiles.lock);
	auto it = gTurnstiles.slots.find(ut);
	if (it == gTurnstiles.slots.end() || !it->second.active)
		return;
	it->second.inheritor = THREAD_NULL;
	it->second.wakes++;
	gTurnstiles.events.push_back({HostTurnstiles::Event::WakeAll, ut, THREAD_NULL});
	gTurnstiles.cv.notify_all();
}

inline void switch_user_turnstile_thread_retain(thread_t thread)
{
	gTurnstiles.retain(thread);
}

inline void switch_user_turnstile_thread_drop(thread_t thread)
{
	gTurnstiles.drop(thread);
}

// No thread of these tests waits on the request-list condition: a sleep is an abort
inline int IOLockSleep(IOLock *, void *, int)
{
	return THREAD_INTERRUPTED;
}

inline void IOLockWakeup(IOLock *, void *, bool)
{
}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

#define IONew(type, count) static_cast<type *>(calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) free(pointer)

inline bool PE_parse_boot_argn(const char *, void *, int)
{
	return false;
}
