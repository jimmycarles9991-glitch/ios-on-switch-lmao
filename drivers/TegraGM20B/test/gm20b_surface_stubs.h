// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
using task_t = void *;
using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
constexpr int kIOReturnSuccess = 0;
constexpr IOOptionBits kIOMemoryMapperNone = 0, kIOMapAnywhere = 1, kIOMapOverwrite = 2,
		       kIOMapUnique = 4, kIOMapPrefault = 8;
#define PAGE_SIZE 16384
#define OSSafeReleaseNULL(x)            \
	do {                            \
		if (x)                  \
			(x)->release(); \
		(x) = nullptr;          \
	} while (0)

class OSObject
{
public:
	virtual ~OSObject() = default;
	mutable unsigned refs = 1;
	uint8_t fields[0x400] = {};

	void retain() const
	{
		++refs;
	}

	void release() const
	{
		--refs;
	}
};

class OSDictionary : public OSObject
{
};

class IOService : public OSObject
{
public:
	bool inactive = false;

	bool isInactive() const
	{
		return inactive;
	}

	static OSDictionary *nameMatching(const char *)
	{
		static OSDictionary match;
		match.retain();
		return &match;
	}

	static IOService *copyMatchingService(OSDictionary *)
	{
		static IOService root;
		root.retain();
		return &root;
	}
};

class IOUserClient : public IOService
{
};
class IOMemoryMap;

class IOMemoryDescriptor : public OSObject
{
public:
	uint64_t bytes = 0;
	std::vector<uint64_t> pages;
	unsigned prepared = 0, completed = 0, outside = 0, cpuUsers = 0;
	bool exclusive = false;
	uint64_t generation = 1;
	bool failPrepare = false;

	uint64_t getLength() const
	{
		return bytes;
	}

	int prepare()
	{
		if (failPrepare)
			return -1;
		++prepared;
		return 0;
	}

	void complete()
	{
		++completed;
	}

	uint64_t getPhysicalSegment(uint64_t offset, IOByteCount *length, IOOptionBits)
	{
		if (offset >= bytes || offset / 4096 >= pages.size()) {
			++outside;
			*length = 0;
			return 0;
		}
		*length = std::min(bytes - offset, 4096 - offset % 4096);
		return pages[offset / 4096] + offset % 4096;
	}

	IOMemoryMap *createMappingInTask(task_t, uint64_t, IOOptionBits, uint64_t, uint64_t)
	{
		return nullptr;
	}
};

class IOMemoryMap : public OSObject
{
public:
	IOMemoryDescriptor *getMemoryDescriptor() const
	{
		return nullptr;
	}

	uint64_t getLength() const
	{
		return 0;
	}

	task_t getAddressTask() const
	{
		return nullptr;
	}

	uint64_t getAddress() const
	{
		return 0;
	}

	IOOptionBits getMapOptions() const
	{
		return 0;
	}
};

using lck_mtx_t = std::mutex;

inline void lck_mtx_lock(lck_mtx_t *lock) __attribute__((no_thread_safety_analysis))
{
	lock->lock();
}

inline void lck_mtx_unlock(lck_mtx_t *lock) __attribute__((no_thread_safety_analysis))
{
	lock->unlock();
}

inline void IOLog(const char *, ...)
{
}

namespace OriginalIOSurface
{
constexpr unsigned long baseOffset = 0x168, clientMemory = 0x150, parent = 0x358,
			memoryRegions = 0x350;
}

namespace OriginalIOSurfaceClient
{
constexpr unsigned long sharedClient = 0x10, map = 0x20;
}

struct OriginalIOSurfaceRootUserClient {
	static constexpr unsigned long clientTableLock = 0x20, clientTable = 0x28,
				       clientCapacity = 0x30, sharedClient = 0x38;
};

class SwitchMemoryAttachment
{
public:
	static int enterExclusive(IOMemoryDescriptor *memory, bool)
	{
		if (memory->exclusive || memory->cpuUsers)
			return -16;
		memory->exclusive = true;
		memory->retain();
		return 0;
	}

	static void leaveExclusive(IOMemoryDescriptor *memory)
	{
		memory->exclusive = false;
		memory->release();
	}

	static uint32_t cpuUsers(IOMemoryDescriptor *memory)
	{
		return memory ? memory->cpuUsers : 0;
	}

	static volatile uint64_t *generation(IOMemoryDescriptor *memory)
	{
		return memory ? &memory->generation : nullptr;
	}

	static int enterCpu(IOMemoryDescriptor *memory, bool)
	{
		if (memory->exclusive)
			return -16;
		++memory->cpuUsers;
		memory->retain();
		return 0;
	}

	static void leaveCpu(IOMemoryDescriptor *memory)
	{
		--memory->cpuUsers;
		memory->release();
	}
};
