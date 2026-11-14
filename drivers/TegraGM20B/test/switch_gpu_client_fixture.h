// SPDX-License-Identifier: MIT
// AGXDeviceUserClient's retirement, binds and BO publication (SwitchGpuClientRetirement.h, SwitchGpuClientBinds.h) over
// stand-ins for the GPU's page tables and channels, and a client of only the members those use.
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "bazel/testing/require.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/SwitchGpuBindings.h"
#include "drivers/TegraGM20B/SwitchGpuBoIndex.h"
#include "drivers/TegraGM20B/SwitchGpuBindingTable.h"
#define SWITCH_GPU_OWNER_COUNTS 1  // the debug kernel's BO counts (SwitchGpuOwnerMemory.h)
#include "drivers/TegraGM20B/SwitchGpuOwnerMemory.h"
#include "drivers/TegraGM20B/TegraGM20BBindHistory.h"
#include "gm20b_gpu_memory.h"

using namespace std::chrono_literals;
constexpr uint32_t kMaxChannels = 8;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -2, kIOReturnNoMemory = -3,
		   kIOReturnNotOpen = -4;
constexpr int THREAD_UNINT = 0;

struct IOLock {
	std::mutex mutex;
	std::condition_variable changed;
};

void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
}

void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

bool IOLockTryLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	return lock->mutex.try_lock();
}

void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
	lock->changed.wait(held);
	held.release();
}

void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->changed.notify_all();
}

#define IODelete(p, type, count) \
	do {                     \
		(void)(count);   \
		delete (p);      \
		(p) = nullptr;   \
	} while (0)
using thread_call_param_t = void *;

struct Call {
	std::atomic<unsigned> entries{0};
	bool cancelled = false;
	std::mutex completionLock;
	std::condition_variable completion;
	unsigned running = 0;
};

using thread_call_t = Call *;
std::function<void()> onCallEnter;

void thread_call_enter(Call *call)
{
	REQUIRE(call != nullptr);
	if (onCallEnter)
		onCallEnter();
	++call->entries;
}

void thread_call_cancel_wait(Call *call)
{
	std::unique_lock held(call->completionLock);
	call->completion.wait(held, [&] {
		return !call->running;
	});
	call->cancelled = true;
}

struct tgm_chan {
	uint32_t completed = 0, submitted = 1, queue = 1;
	uint64_t generation = 1, fault = 0;
	int error = 0;
};

uint64_t tgm_chan_fault_va(tgm_chan *chan)
{
	return chan->fault;
}

std::vector<std::string> logs;

void IOLog(const char *format, ...)
{
	char line[256];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	logs.emplace_back(line);
}

IOReturn ioreturn(int error)
{
	return error;
}

void switch_gpu_chan_next_token(tgm_chan *chan, switch_gpu_backing_token *out)
{
	*out = {uint32_t(chan->submitted + 1), chan->generation, SWITCH_GPU_ENGINE_GM20B,
		chan->queue};
}

int tgm_chan_error(tgm_chan *chan)
{
	return chan->error;
}

uint32_t tgm_chan_completed(tgm_chan *chan)
{
	return chan->completed;
}

uint32_t tgm_chan_submitted(tgm_chan *chan)
{
	return chan->submitted;
}

// The seqnos a retiring binding asked its channels' interrupts for (quiet job ends, SwitchGpuQuiet.h)
std::vector<uint32_t> asked;

void tgm_chan_need(tgm_chan *, uint32_t seqno)
{
	asked.push_back(seqno);
}

void tgm_chan_del(tgm_chan **chan)
{
	*chan = nullptr;
}

// One run of PTEs over a test buffer, as the GPU sees it through Memory (gm20b_ce_model.h): a pending one has none yet,
// a big one is a 128 KiB page an edge may cut.
struct switch_gpu_mapping {
	unsigned id;
	uint64_t va = 0, bytes = 0;
	std::vector<uint8_t> *buffer = nullptr;
	uint64_t offset = 0;
	bool pending = false, big = false, vol = false;
};

struct tgm_mem {
	std::vector<uint8_t> *buffer;
};

Memory gpuMemory;
// The stand-in page tables: maps come under the connection lock, unmaps under the unmap lock
std::mutex gpuMemoryLock;

void installPtes(switch_gpu_mapping *m)
{
	std::lock_guard<std::mutex> held(gpuMemoryLock);
	if (m->buffer)
		gpuMemory.map(m->va, m->buffer->data() + m->offset, m->bytes, false, !m->vol);
	m->pending = false;
}

// A mapping cut as SwitchGpuVmMap.h cuts one: the parts below, in and above the range, each NULL when empty.
std::vector<std::pair<uint64_t, uint64_t>> splits;
int splitError;

int switch_gpu_vm_split(void *, switch_gpu_mapping **mapping, tgm_mem *, uint8_t, int, int,
			uint64_t va, uint64_t range, switch_gpu_mapping **below,
			switch_gpu_mapping **taken, switch_gpu_mapping **above)
{
	if (splitError)
		return splitError;
	const auto *old = *mapping;
	const uint64_t end = va + range, oldEnd = old->va + old->bytes;
	const uint64_t low = std::max(old->va, va), high = std::min(oldEnd, end);
	const bool keep = old->big && (old->va < va || oldEnd > end);
	const auto part = [&](uint64_t from, uint64_t to) {
		return new switch_gpu_mapping{old->id,
					      from,
					      to - from,
					      old->buffer,
					      old->offset + (from - old->va),
					      old->pending || keep,
					      false};
	};
	*below = old->va < va ? part(old->va, va) : nullptr;
	*taken = keep && !old->pending ? new switch_gpu_mapping(*old) : part(low, high);
	(*taken)->pending = old->pending;
	*above = oldEnd > end ? part(end, oldEnd) : nullptr;
	splits.push_back({va, range});
	delete *mapping;
	*mapping = nullptr;
	return 0;
}

int switch_gpu_mapping_span(const switch_gpu_mapping *mapping, uint64_t *va, uint64_t *bytes)
{
	if (!mapping)
		return -22;
	*va = mapping->va;
	*bytes = mapping->bytes;
	return 0;
}

void switch_gpu_mapping_describe(const switch_gpu_mapping *mapping, char *out, size_t size)
{
	snprintf(out, size, "%#llx+%#llx/12", (unsigned long long)mapping->va,
		 (unsigned long long)mapping->bytes);
}

std::function<void(switch_gpu_mapping *)> onUnmap;
std::vector<unsigned> unmapped;

int switch_gpu_mapping_pending(const switch_gpu_mapping *mapping)
{
	return mapping && mapping->pending;
}

int switch_gpu_mapping_installed(const switch_gpu_mapping *mapping)
{
	return mapping && !mapping->pending;
}

int installError;

int switch_gpu_vm_install(void *, switch_gpu_mapping *mapping, tgm_mem *, uint8_t, int, int vol)
{
	if (installError)
		return installError;
	mapping->vol = vol != 0;
	if (mapping->pending)
		installPtes(mapping);
	return 0;
}

int switch_gpu_vm_volatile_kind_ok(uint8_t, int)
{
	return 1;
}

int switch_gpu_vm_prepare(void *, uint64_t va, uint64_t range, tgm_mem *mem, uint64_t offset,
			  uint8_t, switch_gpu_mapping **out)
{
	*out = new switch_gpu_mapping{9000, va, range, mem->buffer, offset, true, false};
	return 0;
}

int switch_gpu_vm_map_volatile(void *, uint64_t va, uint64_t range, tgm_mem *mem, uint64_t offset,
			       uint8_t, int, int vol, switch_gpu_mapping **out)
{
	*out = new switch_gpu_mapping{9000, va, range, mem->buffer, offset, true, false, vol != 0};
	installPtes(*out);
	return 0;
}

const volatile uint32_t *bindWord;
uint32_t bindTarget;
std::function<void()> onBindPublished;

void tgm_vm_bind_wait(void *, uint64_t, const volatile uint32_t *cpu, uint32_t target)
{
	bindWord = cpu;
	bindTarget = target;
	if (onBindPublished)
		onBindPublished();
}

// What a submission made now would wait for on the GPU: the bind epoch not reached yet.
bool newWorkBlocked()
{
	return bindWord && int32_t(*bindWord - bindTarget) < 0;
}

void switch_gpu_vm_unmap(void *, switch_gpu_mapping **mapping)
{
	if (!*mapping)
		return;
	if (!(*mapping)->pending) {
		std::lock_guard<std::mutex> held(gpuMemoryLock);
		gpuMemory.unmap((*mapping)->va, (*mapping)->bytes);
	}
	if (onUnmap)
		onUnmap(*mapping);
	{
		std::lock_guard<std::mutex> held(gpuMemoryLock);
		unmapped.push_back((*mapping)->id);
	}
	delete *mapping;
	*mapping = nullptr;
}

uint64_t nowNs()
{
	return 0;
}

void tgm_stats_bo_free(uint64_t)
{
}

void scheduleOrphanRelease()
{
}

using thread_t = void *;

unsigned membersFreed = 0;

struct switch_gpu_resource_set;
// The BOs' backings that are a resource set's members
std::vector<switch_gpu_backing *> setMembers;

switch_gpu_resource_set *switch_gpu_backing_set(switch_gpu_backing *backing)
{
	return backing && std::find(setMembers.begin(), setMembers.end(), backing) !=
				       setMembers.end()
		       ? reinterpret_cast<switch_gpu_resource_set *>(backing)
		       : nullptr;
}

void switch_gpu_backing_member_freed(switch_gpu_backing *)
{
	++membersFreed;
}

thread_t current_thread()
{
	return nullptr;
}

constexpr uint64_t kAlive = 0xa11fe;

class AGXDeviceUserClient
{
public:
	struct Bo {
		uint64_t canary = kAlive;
		Bo *next = nullptr;
		uint64_t handle = 1, bytes = 4096, controlCharge = 0;
		void *pool_va = nullptr;
		bool chunked = false, readOnly = false;
		tgm_mem *mem = nullptr;
		uint32_t bindings = 0, flags = 0;
		Bo **zombieLink = nullptr;
		switch_gpu_backing *backing = nullptr;
		SwitchGpuOwnerKind ownerKind = SWITCH_GPU_OWNER_IMPORTED;
	};

	struct RetiredBinding {
		RetiredBinding *next;
		uint64_t va, range;
		Bo *bo;
		switch_gpu_mapping *mapping;
		switch_gpu_backing_token snapshots[kMaxChannels];
		uint8_t kind, vol;
		uint32_t epoch;
		bool unmapped;
	};

	struct Binding {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
		RetiredBinding *retirement;
	};

	struct Gate {
		bool value = false, mutation = false;
		unsigned mutations = 0;

		bool closed() const
		{
			return value;
		}

		void close(IOLock *)
		{
			value = true;
		}

		bool beginMutation(IOLock *)
		{
			++mutations;
			mutation = true;
			return !value;
		}

		void endMutation(IOLock *)
		{
			mutation = false;
		}

		bool mutating() const
		{
			return mutation;
		}
	} submitGate_;

	struct Surfaces {
		void clear()
		{
		}

		void remove(uint64_t)
		{
		}
	} surfaces_;

	struct Service {
		void stop()
		{
		}
	} events_, resources_;

	struct EventChannel {
		void reset()
		{
		}
	} eventChannels_[kMaxChannels];

	struct Admission {
		void end()
		{
		}
	} admissions_[kMaxChannels];

	uint64_t admissionCookies_[kMaxChannels] = {};
	IOLock storage;
	IOLock *lock_ = &storage;
	void *vm_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr, *readyBos_ = nullptr;

	struct HostMemory {
		template <typename T> static T *allocate(uint32_t count)
		{
			return static_cast<T *>(malloc(sizeof(T) * count));
		}

		template <typename T> static void release(T *items, uint32_t)
		{
			free(items);
		}
	};

	using BoIndex = SwitchGpuBoIndex<Bo, HostMemory>;

	struct Index : BoIndex {
		~Index()
		{
			destroy();
		}
	} boIndex_;

	using BindingTable = SwitchGpuBindingTable<Bo, HostMemory>;

	struct Bindings : BindingTable {
		~Bindings()
		{
			destroy();
		}
	} bindTable_;

	struct Left {
		Bo *bos;
		BoIndex::Table *indexes;
		BindingTable::Snapshot *bindings;
	};

	Left left_ = {};
	Call releaseStorage;
	thread_call_t releaseCall_ = &releaseStorage;
	IOLock submitStorage[kMaxChannels];
	IOLock *submitLocks_[kMaxChannels] = {
		&submitStorage[0], &submitStorage[1], &submitStorage[2], &submitStorage[3],
		&submitStorage[4], &submitStorage[5], &submitStorage[6], &submitStorage[7]};

	RetiredBinding *retired_ = nullptr, **retiredTail_ = &retired_, *unmapping_ = nullptr;
	IOLock unmapStorage;
	IOLock *unmapLock_ = &unmapStorage;
	bool faultLogged_[kMaxChannels] = {};
	static constexpr uint32_t kBindEpochWord = 256;
	uint32_t bindIssued_ = 0, bindDone_ = 0;
	uint32_t fencePage[1024] = {};
	volatile uint32_t *fence_va_ = fencePage;
	uint64_t fence_gpu_ = 0x1000;
	char owner_[17] = "test";
	Call call;
	thread_call_t retireCall_ = &call;
	Call wakeStorage;
	thread_call_t wakeCall_ = &wakeStorage;
	uint32_t promisesKept_ = 0;
	uint32_t retiredCount_ = 0, retireRemaining_ = 0, retirementPending_ = 0,
		 retirementStopped_ = 0;
	uint64_t retirementEpoch_ = 0, retireScanEpoch_ = 0, pool_bytes_ = 0, control_bytes_ = 0;
	SwitchGpuOwnerMemory ownerMemory_ = {};
	Binding binds_[256] = {};
	uint32_t nbinds_ = 0;
	thread_t hangHolder_ = nullptr;
	std::atomic<unsigned> releases{0};
	unsigned waits = 0;
	std::function<void()> onWait, onRelease;

	void releaseMappings()
	{
	}

	void releaseCpuLeases()
	{
	}

	void destroySets()
	{
	}

	void releasePromises(bool)
	{
	}

	void releaseBo(Bo *bo)
	{
		if (onRelease)
			onRelease();
		++releases;
		bo->canary = 0;
		delete bo;
	}

	IOReturn waitFence(const TegraGM20BAbiWaitFence *wait)
	{
		++waits;
		if (onWait)
			onWait();
		chans_[wait->channel - 1]->completed = wait->seqno;
		retirementFence(this);
		retireBatch();
		return 0;
	}

	void accountRelease(Bo *);
	void readyBo(Bo *);
	void queueZombie(Bo *);
	void scheduleRetirement();
	static void retirementFence(void *);
	static void retirementWork(thread_call_param_t, thread_call_param_t);
	bool bindingIdle(const RetiredBinding *) const;
	void retireBatch();
	void takeRetiredOverlaps(uint64_t, uint64_t);
	void retire(RetiredBinding *);
	IOReturn cutOverlaps(uint64_t, uint64_t, bool, bool, RetiredBinding **);
	bool workQueued(uint64_t, uint64_t) const;
	IOReturn mapRange(const TegraGM20BAbiBindOp &, Bo *, RetiredBinding *, bool);
	uint32_t issueBind();
	void installPendingBinds();
	void logFaultBindings();
	void noteBind(uint8_t, uint64_t, uint64_t, const Bo *, uint64_t, uint8_t);
	void describeFault(uint32_t, uint64_t);
	void reap(bool);
	void drainResources();
	IOReturn boFree(const TegraGM20BAbiBoFree *);
	Bo **findLink(uint64_t);
	IOReturn freeBo(Bo **);
	void unbindOverlaps(uint64_t, uint64_t, Bo *);
	void publishBindings(BindingTable::Snapshot *);
	bool passSubmitters(bool);
	void dropLeft(const Left &);
	void keepLeft(const Left &);
	void releaseLeft();
	static void releaseWork(thread_call_param_t, thread_call_param_t);

	Bo *findBo(uint64_t handle)
	{
		return boIndex_.find(handle);
	}

	Bo *add(unsigned id, uint64_t va = 0x10000)
	{
		return add(id, va, 4096);
	}

	Bo *add(unsigned id, uint64_t va, uint64_t bytes, std::vector<uint8_t> *buffer = nullptr,
		bool big = false)
	{
		auto *bo = new Bo;
		bo->handle = id;
		bo->bytes = bytes;
		bo->bindings = 1;
		bo->mem = new tgm_mem{buffer};
		bo->next = bos_;
		bos_ = bo;
		auto *mapping = new switch_gpu_mapping{id, va, bytes, buffer, 0, false, big};
		installPtes(mapping);
		auto *record =
			new RetiredBinding{nullptr, va, bytes, bo, mapping, {}, 0, 0, 0, false};
		const uint32_t at = switch_gpu_binding_upper(binds_, nbinds_, va);
		memmove(&binds_[at + 1], &binds_[at], sizeof(Binding) * (nbinds_ - at));
		binds_[at] = {va, bytes, bo, 0, record};
		++nbinds_;
		return bo;
	}

	// VmBind's steps for one op (TegraGM20BUserClientChannel.cpp vmBind, bindOp): a Map of `bo` or, with no BO, an
	// Unmap, then the bindings published.
	IOReturn bind(uint64_t va, uint64_t range, Bo *bo, uint32_t mapOp = TegraGM20BBindMap)
	{
		RetiredBinding *spare[2] = {new RetiredBinding{}, new RetiredBinding{}};
		IOLockLock(lock_);
		BindingTable::Snapshot *snapshot = BindingTable::prepare(nbinds_ + 2);
		const bool queued = workQueued(va, range);
		IOReturn result = cutOverlaps(va, range, !queued, bo != nullptr, spare);
		if (!result && bo) {
			TegraGM20BAbiBindOp op = {};
			op.op = mapOp;
			op.handle = bo->handle;
			op.va = va;
			op.range = range;
			result = mapRange(op, bo, new RetiredBinding{}, queued);
		}
		publishBindings(snapshot);
		IOLockUnlock(lock_);
		for (auto *each : spare)
			delete each;
		return result;
	}
};

#include "drivers/TegraGM20B/SwitchGpuClientRetirement.h"
