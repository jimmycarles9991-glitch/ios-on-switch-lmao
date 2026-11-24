// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define IONew(type, count) static_cast<type *>(calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) free(pointer)

using IOReturn = int;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
using IODirection = uint32_t;
using kern_return_t = int;
using boolean_t = int;
using ppnum_t = uint32_t;
using integer_t = int;
using wait_result_t = int;
using upl_offset_t = uint32_t;
using upl_size_t = uint32_t;
using caddr_t = char *;
using task_t = struct switch_pack_task *;
using thread_t = struct switch_pack_thread *;
using vnode_t = struct switch_pack_vnode *;
using vfs_context_t = struct switch_pack_vfs_ctx *;
using upl_t = struct switch_pack_upl *;
using thread_continue_t = void (*)(void *, wait_result_t);

constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnError = -1;
constexpr IOReturn kIOReturnNoMemory = -2;
constexpr IOReturn kIOReturnBadArgument = -3;
constexpr IOReturn kIOReturnAborted = -4;
constexpr IOReturn kIOReturnNotPermitted = -5;
constexpr IOReturn kIOReturnNotOpen = -6;
constexpr IOReturn kIOReturnExclusiveAccess = -7;

constexpr kern_return_t KERN_SUCCESS = 0;
constexpr kern_return_t KERN_FAILURE = 5;

constexpr IOOptionBits kIODirectionIn = 1u << 0;
constexpr IOOptionBits kIODirectionOut = 1u << 1;
constexpr IOOptionBits kIODirectionInOut = kIODirectionIn | kIODirectionOut;
constexpr IOOptionBits kIOMemoryMapperNone = 1u << 8;
constexpr IOOptionBits kIOMemoryPhysicallyContiguous = 1u << 9;
constexpr IOOptionBits kIOMapAnywhere = 1u << 0;
constexpr IOOptionBits kIOMapReadOnly = 1u << 1;

constexpr int THREAD_UNINT = 0;
constexpr int THREAD_ABORTSAFE = 1;
constexpr int THREAD_AWAKENED = 0;
constexpr int THREAD_INTERRUPTED = 2;

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 14
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (1u << PAGE_SHIFT)
#endif
#ifndef FREAD
#define FREAD 0x0001
#endif
#ifndef O_NOFOLLOW_ANY
#define O_NOFOLLOW_ANY 0x20000000
#endif
#ifndef UIO_READ
#define UIO_READ 0
#endif
#ifndef UIO_SYSSPACE
#define UIO_SYSSPACE 2
#endif
#ifndef IO_NOAUTH
#define IO_NOAUTH 0x10
#endif
#ifndef UPL_IOSYNC
#define UPL_IOSYNC 0x1
#endif
#ifndef UPL_FILE_IO
#define UPL_FILE_IO 0x400
#endif
#ifndef UPL_SET_LITE
#define UPL_SET_LITE 0x100
#endif
#ifndef UPL_COPYOUT_FROM
#define UPL_COPYOUT_FROM 0x200
#endif
#ifndef UPL_NOBLOCK
#define UPL_NOBLOCK 0x800
#endif
#ifndef UPL_COMMIT_FREE_ON_EMPTY
#define UPL_COMMIT_FREE_ON_EMPTY 0x1
#endif
#ifndef UPL_COMMIT_INACTIVATE
#define UPL_COMMIT_INACTIVATE 0x8
#endif
#ifndef UPL_COMMIT_CLEAR_PRECIOUS
#define UPL_COMMIT_CLEAR_PRECIOUS 0x80
#endif
#ifndef UPL_ABORT_FREE_ON_EMPTY
#define UPL_ABORT_FREE_ON_EMPTY 0x8
#endif
#ifndef IOPOL_THROTTLE
#define IOPOL_THROTTLE 3
#endif
#ifndef MAXPRI_THROTTLE
#define MAXPRI_THROTTLE 4
#endif

struct switch_pack_task {
	uint32_t id;
};

extern task_t kernel_task;

struct IOLock {
	std::mutex mutex;
	std::condition_variable cv;
};

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

int switch_pack_test_sleep(IOLock *lock, void *event, int interruptible);

inline int IOLockSleep(IOLock *lock, void *event, int interruptible)
{
	return switch_pack_test_sleep(lock, event, interruptible);
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->cv.notify_all();
}

inline void bzero(void *p, size_t n)
{
	std::memset(p, 0, n);
}

#ifndef __APPLE__
inline size_t strlcpy(char *dst, const char *src, size_t size)
{
	const size_t len = std::strlen(src);
	if (size) {
		const size_t copy = len < size - 1 ? len : size - 1;
		std::memcpy(dst, src, copy);
		dst[copy] = '\0';
	}
	return len;
}
#endif

uint64_t mach_absolute_time();
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void IOLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct upl_page_info_t {
	ppnum_t phys_addr;
	bool present;
	bool valid;
};

struct vnode_attr {
	uint64_t va_data_size;
	bool sizeSupported;
};

#define VATTR_INIT(va) std::memset((va), 0, sizeof(*(va)))
#define VATTR_WANTED(va, field) ((void)(va))
#define VATTR_IS_SUPPORTED(va, field) ((va)->sizeSupported)

class IOMemoryMap
{
public:
	uint64_t address = 0;
	uint64_t size = 0;
	bool unmapped = false;
	std::atomic<int> refs{1};
	std::vector<uint8_t> *backing = nullptr;
	std::function<void()> onRelease;

	uint64_t getAddress() const
	{
		return address;
	}

	uint64_t getSize() const
	{
		return size;
	}

	void unmap()
	{
		unmapped = true;
	}

	void retain()
	{
		refs.fetch_add(1);
	}

	void release()
	{
		if (refs.fetch_sub(1) == 1) {
			unmapped = true;
			if (onRelease)
				onRelease();
			delete this;
		}
	}
};

class IOMemoryDescriptor
{
public:
	std::atomic<int> refs{1};
	int preparedCount = 0;
	IOOptionBits preparedDir = 0;
	uint64_t length = 0;
	bool failPrepare = false;

	virtual ~IOMemoryDescriptor() = default;

	virtual void retain()
	{
		refs.fetch_add(1);
	}

	virtual void release()
	{
		if (refs.fetch_sub(1) == 1)
			delete this;
	}

	virtual IOReturn prepare(IODirection dir = kIODirectionNone)
	{
		if (failPrepare)
			return kIOReturnBadArgument;
		++preparedCount;
		preparedDir = dir;
		return kIOReturnSuccess;
	}

	virtual IOReturn complete(IODirection = kIODirectionNone)
	{
		--preparedCount;
		return kIOReturnSuccess;
	}

	virtual addr64_t getPhysicalSegment(IOByteCount, IOByteCount *len, IOOptionBits = 0)
	{
		if (len)
			*len = 0;
		return 0;
	}

	virtual IOMemoryMap *createMappingInTask(task_t task, uint64_t at, IOOptionBits options);

	static constexpr IODirection kIODirectionNone = 0;
	static IOMemoryDescriptor *withAddressRange(uint64_t address, uint64_t bytes,
						    IOOptionBits options, task_t task);
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor
{
public:
	std::vector<uint8_t> storage;
	std::vector<uint64_t> pagePas;
	bool contiguous = false;

	void *getBytesNoCopy()
	{
		return storage.data();
	}

	addr64_t getPhysicalSegment(IOByteCount offset, IOByteCount *len, IOOptionBits = 0) override
	{
		if (offset >= length || pagePas.empty()) {
			if (len)
				*len = 0;
			return 0;
		}
		const size_t idx = size_t(offset >> 12);
		const uint64_t base = pagePas[idx];
		size_t run = 1;
		while (idx + run < pagePas.size() && pagePas[idx + run] == base + run * 4096ULL)
			++run;
		if (len)
			*len = uint64_t(run) * 4096ULL;
		return base;
	}

	static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(task_t task, IOOptionBits options,
								uint64_t capacity, uint64_t mask);
};

class IOMultiMemoryDescriptor : public IOMemoryDescriptor
{
public:
	std::vector<IOMemoryDescriptor *> parts;

	~IOMultiMemoryDescriptor() override
	{
		for (auto *p : parts)
			p->release();
	}

	IOReturn prepare(IODirection dir = kIODirectionNone) override
	{
		if (failPrepare)
			return kIOReturnNoMemory;
		for (size_t i = 0; i < parts.size(); ++i) {
			if (parts[i]->prepare(dir) != kIOReturnSuccess) {
				for (size_t j = 0; j < i; ++j)
					parts[j]->complete(dir);
				return kIOReturnNoMemory;
			}
		}
		++preparedCount;
		return kIOReturnSuccess;
	}

	IOReturn complete(IODirection dir = kIODirectionNone) override
	{
		for (auto *p : parts)
			p->complete(dir);
		--preparedCount;
		return kIOReturnSuccess;
	}

	addr64_t getPhysicalSegment(IOByteCount offset, IOByteCount *len,
				    IOOptionBits opts = 0) override
	{
		uint64_t cursor = 0;
		for (auto *p : parts) {
			if (offset < cursor + p->length)
				return p->getPhysicalSegment(offset - cursor, len, opts);
			cursor += p->length;
		}
		if (len)
			*len = 0;
		return 0;
	}

	static IOMultiMemoryDescriptor *withDescriptors(IOMemoryDescriptor **descriptors,
							uint32_t count, IODirection, bool);
};

extern "C" {
int vnode_open(const char *path, int fmode, int cmode, int flags, vnode_t *vpp, vfs_context_t ctx);
int vnode_close(vnode_t vp, int flags, vfs_context_t ctx);
int vnode_isreg(vnode_t vp);
int vnode_getattr(vnode_t vp, struct vnode_attr *vap, vfs_context_t ctx);
int vn_rdwr(int rw, vnode_t vp, caddr_t base, int len, off_t offset, int segflg, int ioflg,
	    void *cred, int *aresid, void *p);
void *vfs_context_ucred(vfs_context_t ctx);
void *vfs_context_proc(vfs_context_t ctx);
vfs_context_t vfs_context_kernel(void);

int vnode_pagein(vnode_t vp, upl_t upl, upl_offset_t upl_offset, uint64_t f_offset, upl_size_t size,
		 int flags, int *errorp);
kern_return_t ubc_create_upl_external(vnode_t vp, off_t f_offset, int bufsize, upl_t *uplp,
				      upl_page_info_t **plp, int uplflags);
kern_return_t ubc_upl_commit(upl_t upl);
kern_return_t ubc_upl_commit_range(upl_t upl, upl_offset_t offset, upl_size_t size, int flags);
kern_return_t ubc_upl_abort_range(upl_t upl, upl_offset_t offset, upl_size_t size, int flags);
ppnum_t upl_phys_page(upl_page_info_t *upl, int index);
boolean_t upl_page_present(upl_page_info_t *upl, int index);
boolean_t upl_valid_page(upl_page_info_t *upl, int index);

kern_return_t kernel_thread_start(thread_continue_t cont, void *param, thread_t *out);
kern_return_t kernel_thread_start_priority(thread_continue_t cont, void *param, integer_t prio,
					   thread_t *out);
void thread_deallocate(thread_t thread);
thread_t current_thread(void);
kern_return_t thread_terminate(thread_t thread);
void throttle_set_thread_io_policy(int policy);
extern int memorystatus_vm_pressure_level;
}

void switch_pack_test_dcache_op(const char *op, uintptr_t address);
#define SWITCH_DCACHE_OP(op, address) switch_pack_test_dcache_op(op, address)
#define SWITCH_DCACHE_BARRIER() ((void)0)
