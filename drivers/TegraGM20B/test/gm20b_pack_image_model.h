// SPDX-License-Identifier: MIT
// The kernel model SwitchGpuPackImage.cpp runs on in host tests: its file, page cache, UPLs, memory, threads and copy
// engine, and the test fixture that resets them
#pragma once
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/SwitchGpuCachedZero.h"
#include "drivers/TegraGM20B/SwitchGpuMapping.h"
#include "drivers/TegraGM20B/SwitchGpuPackImage.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20BMetal/pack.h"
#include "switch_gpu_pack_image_stubs.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

struct tgm_vm {
	uint32_t id = 0;
	std::map<uint64_t, switch_gpu_mapping *> mappings;
};

struct tgm_mem {
	const uint64_t *pages = nullptr;
	uint32_t npages = 0;
	bool freed = false;
};

struct switch_gpu_mapping {
	uint64_t va = 0;
	uint64_t bytes = 0;
	tgm_mem *mem = nullptr;
	int readOnly = 0;
	int vol = 0;
	uint64_t bigBytes = 0;
};

struct switch_pack_vnode {
	std::string path;
	bool closed = false;
};

struct switch_pack_vfs_ctx {};

struct switch_pack_upl {
	uint64_t offset = 0;
	uint32_t size = 0;
	std::vector<upl_page_info_t> pl;
	std::vector<bool> released;
	bool deallocated = false;
};

struct switch_pack_thread {
	std::thread th;
};

static switch_pack_task gKernelTask = {0};
task_t kernel_task = &gKernelTask;
int memorystatus_vm_pressure_level = 0;

namespace
{
struct Model {
	std::mutex mu;
	std::condition_variable cv;
	std::vector<uint8_t> fileBytes;
	bool fileExists = true;
	bool fileRegular = true;
	bool failOpen = false;
	uint32_t openCount = 0;
	uint32_t closeCount = 0;
	int openVnodes = 0;
	int liveDescriptors = 0;
	int liveMaps = 0;
	int activeUpls = 0;
	int maxConcurrentUpls = 0;
	uint32_t committedUpls = 0;
	uint32_t abortedUpls = 0;
	uint32_t copyCalls = 0;
	uint32_t pageinCalls = 0;
	uint32_t srcCleanCalls = 0;
	uint32_t dstCleanInvalOps = 0;
	uint32_t dstInvalOps = 0;
	uint64_t nextPa = 0x80000000ULL;
	uint64_t nextUserVa = 0x200000000ULL;
	uint64_t clockNs = 1000000ULL;
	std::map<uint64_t, uint8_t *> physToHost;
	std::map<uint64_t, uint64_t> pageCachePa;  // fileOff (16 KiB aligned) -> phys addr
	std::map<std::pair<IOMemoryDescriptor *, task_t>, IOMemoryMap *> taskMaps;
	std::vector<std::vector<uint8_t>> pageCacheStorage;
	std::vector<std::string> logs;
	std::vector<std::thread> detachedThreads;

	// Fault injection knobs
	int failContigChunkIndex = -1;  // chunk index where contiguous alloc returns nullptr
	int allocChunkCounter = 0;
	int failCopyOnCall = -1;
	int copyError = -5;
	int failPageinOnCall = -1;
	bool injectPartialAbsentOnce = false;
	bool injectAlwaysAbsentUpl = false;
	int failWireRangeIndex = -1;
	int wireRangeCounter = 0;
	bool pauseFirstPagein = false;
	bool firstPageinEntered = false;
	bool releaseFirstPagein = false;
	uint32_t sleepers = 0;
	uint32_t unintSleepers = 0;
	bool shutdownDeadlinePassed = false;
	IOLock *panicLock =
		nullptr;  // the lock a timed-out sleep returned holding, which panic() lets go of
	// The pack image's stream: page-ins of its heap (past the header's pages), the heap page-in to hold at (1 its first,
	// 0 none), the kernel fence page's words its frontier is one of, and the failed waits recovered
	uint32_t heapPageins = 0;
	uint32_t pauseHeapPagein = 0;
	bool callPaused = false, releaseCall = false;
	uint32_t fenceWords[64] = {};
	std::vector<std::string> recovered;
	// holdRecovery: recovery waits up to 200 ms for shutdownReturned, then notes whether it ran after
	// switch_gpu_pack_shutdown returned
	bool holdRecovery = false, shutdownReturned = false, recoveredAfterShutdown = false;
	// The boot replay's class (switch_vm_prefetch_replay_thread): whether a thread is in it, its marks, and the page-ins
	// made outside it
	bool replaying = false;
	uint32_t replayMarks = 0, pageinsOutsideReplay = 0;

	void reset()
	{
		joinThreads();
		std::lock_guard<std::mutex> g(mu);
		fileBytes.clear();
		fileExists = true;
		fileRegular = true;
		failOpen = false;
		openCount = closeCount = 0;
		openVnodes = 0;
		liveDescriptors = 0;
		liveMaps = 0;
		activeUpls = maxConcurrentUpls = 0;
		committedUpls = abortedUpls = 0;
		copyCalls = pageinCalls = srcCleanCalls = 0;
		dstCleanInvalOps = dstInvalOps = 0;
		nextPa = 0x80000000ULL;
		nextUserVa = 0x200000000ULL;
		clockNs = 1000000ULL;
		physToHost.clear();
		pageCachePa.clear();
		taskMaps.clear();
		pageCacheStorage.clear();
		logs.clear();
		failContigChunkIndex = -1;
		allocChunkCounter = 0;
		failCopyOnCall = -1;
		copyError = -5;
		failPageinOnCall = -1;
		injectPartialAbsentOnce = false;
		injectAlwaysAbsentUpl = false;
		failWireRangeIndex = -1;
		wireRangeCounter = 0;
		pauseFirstPagein = false;
		firstPageinEntered = false;
		releaseFirstPagein = false;
		heapPageins = 0;
		pauseHeapPagein = 0;
		callPaused = releaseCall = false;
		std::memset(fenceWords, 0, sizeof(fenceWords));
		recovered.clear();
		holdRecovery = shutdownReturned = recoveredAfterShutdown = false;
		replaying = false;
		replayMarks = pageinsOutsideReplay = 0;
		sleepers = 0;
		unintSleepers = 0;
		shutdownDeadlinePassed = false;
		panicLock = nullptr;
		memorystatus_vm_pressure_level = 0;
	}

	void joinThreads()
	{
		std::vector<std::thread> toJoin;
		{
			std::lock_guard<std::mutex> g(mu);
			toJoin.swap(detachedThreads);
		}
		for (auto &t : toJoin)
			if (t.joinable())
				t.join();
	}
};

Model gModel;
switch_pack_vfs_ctx gKernelCtx;

class TrackedUserRangeDescriptor : public IOMemoryDescriptor
{
public:
	uint64_t userAddr = 0;
	task_t ownerTask = nullptr;

	TrackedUserRangeDescriptor()
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		++gModel.liveDescriptors;
	}

	~TrackedUserRangeDescriptor() override
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		--gModel.liveDescriptors;
	}
};

class TrackedBufferDescriptor : public IOBufferMemoryDescriptor
{
public:
	TrackedBufferDescriptor()
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		++gModel.liveDescriptors;
	}

	~TrackedBufferDescriptor() override
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		--gModel.liveDescriptors;
	}
};

class TrackedMultiDescriptor : public IOMultiMemoryDescriptor
{
public:
	TrackedMultiDescriptor()
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		++gModel.liveDescriptors;
	}

	~TrackedMultiDescriptor() override
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		--gModel.liveDescriptors;
	}
};

std::vector<uint8_t> makePackFile(uint64_t tier0HeapBytes, uint64_t tier1HeapBytes,
				  uint32_t version = kSwitchGpuPackVersion,
				  uint64_t stackKey = 0x123456789abcdef0ULL)
{
	const uint64_t programsOff = 0x1000;
	const uint64_t programsBytes = 0x8000;
	const uint64_t variantsOff = programsOff + programsBytes;
	const uint64_t variantsBytes = 0x8000;
	const uint64_t fastKeysOff = variantsOff + variantsBytes;
	const uint64_t fastKeysBytes = 0x8000;
	const uint64_t heapOff = kSwitchGpuPackTierAlign;
	const uint64_t heapBytes = tier0HeapBytes + tier1HeapBytes;
	const uint64_t totalSize = heapOff + heapBytes;

	std::vector<uint8_t> data(size_t(totalSize), 0);
	SwitchGpuPackHeader hdr = {};
	std::memcpy(hdr.magic, kSwitchGpuPackMagic, sizeof(kSwitchGpuPackMagic));
	hdr.version = version;
	hdr.stack = stackKey;
	hdr.section[kSwitchGpuPackPrograms] = {programsOff, programsBytes};
	hdr.tier0[kSwitchGpuPackPrograms] = 0x4000;
	hdr.section[kSwitchGpuPackVariants] = {variantsOff, variantsBytes};
	hdr.tier0[kSwitchGpuPackVariants] = 0x4000;
	hdr.section[kSwitchGpuPackFastKeys] = {fastKeysOff, fastKeysBytes};
	hdr.tier0[kSwitchGpuPackFastKeys] = 0x4000;
	hdr.section[kSwitchGpuPackHeap] = {heapOff, heapBytes};
	hdr.tier0[kSwitchGpuPackHeap] = tier0HeapBytes;
	std::memcpy(data.data(), &hdr, sizeof(hdr));
	for (uint64_t i = 0; i < heapBytes; ++i)
		data[size_t(heapOff + i)] = uint8_t(((i * 131u) ^ (i >> 8) ^ 0x5au) & 0xffu);
	return data;
}
}  // namespace

uint64_t mach_absolute_time()
{
	std::lock_guard<std::mutex> g(gModel.mu);
	gModel.clockNs += 1000000ULL;
	return gModel.clockNs;
}

void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
	*result = abstime;
}

void IOLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> g(gModel.mu);
	gModel.logs.emplace_back(buf);
}

int switch_pack_test_sleep(IOLock *lock, void *, int interruptible)
{
	std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		++gModel.sleepers;
		if (interruptible == THREAD_UNINT)
			++gModel.unintSleepers;
		gModel.cv.notify_all();
	}
	lock->cv.wait(held);
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		--gModel.sleepers;
		if (interruptible == THREAD_UNINT)
			--gModel.unintSleepers;
	}
	held.release();
	return THREAD_AWAKENED;
}

int switch_pack_test_sleep_deadline(IOLock *lock, void *event, uint64_t, int interruptible)
{
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		if (gModel.shutdownDeadlinePassed) {
			gModel.panicLock = lock;
			return THREAD_TIMED_OUT;
		}
	}
	return switch_pack_test_sleep(lock, event, interruptible);
}

void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result)
{
	*result = mach_absolute_time() + uint64_t(interval) * scale_factor;
}

struct PackPanic {
	std::string message;
};

void panic(const char *fmt, ...) __attribute__((no_thread_safety_analysis))
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	IOLock *held = nullptr;
	{
		std::lock_guard<std::mutex> g(gModel.mu);
		held = gModel.panicLock;
		gModel.panicLock = nullptr;
	}
	if (held)
		held->mutex.unlock();
	throw PackPanic{buf};
}

void switch_pack_test_dcache_op(const char *op, uintptr_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	if (std::strcmp(op, "civac") == 0)
		++gModel.dstCleanInvalOps;
	else if (std::strcmp(op, "ivac") == 0)
		++gModel.dstInvalOps;
}

IOMemoryMap *IOMemoryDescriptor::createMappingInTask(task_t task, uint64_t, IOOptionBits)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	const auto key = std::make_pair(this, task);
	auto it = gModel.taskMaps.find(key);
	if (it != gModel.taskMaps.end()) {
		it->second->retain();
		return it->second;
	}
	auto *map = new IOMemoryMap();
	map->address = gModel.nextUserVa;
	map->size = length;
	gModel.nextUserVa += (length + 0xffffULL) & ~0xffffULL;
	++gModel.liveMaps;
	gModel.taskMaps[key] = map;
	map->onRelease = [key, map] {
		std::lock_guard<std::mutex> g2(gModel.mu);
		auto found = gModel.taskMaps.find(key);
		if (found != gModel.taskMaps.end() && found->second == map)
			gModel.taskMaps.erase(found);
		--gModel.liveMaps;
	};
	return map;
}

IOMemoryDescriptor *IOMemoryDescriptor::withAddressRange(uint64_t address, uint64_t bytes,
							 IOOptionBits, task_t task)
{
	auto *d = new TrackedUserRangeDescriptor();
	d->userAddr = address;
	d->length = bytes;
	d->ownerTask = task;
	std::lock_guard<std::mutex> g(gModel.mu);
	const int idx = gModel.wireRangeCounter++;
	if (gModel.failWireRangeIndex == idx)
		d->failPrepare = true;
	return d;
}

IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithPhysicalMask(task_t,
									   IOOptionBits options,
									   uint64_t capacity,
									   uint64_t mask)
{
	const bool wantContig = (options & kIOMemoryPhysicallyContiguous) != 0;
	if (wantContig) {
		std::lock_guard<std::mutex> g(gModel.mu);
		const int idx = gModel.allocChunkCounter++;
		if (gModel.failContigChunkIndex == idx)
			return nullptr;
	}
	auto *b = new TrackedBufferDescriptor();
	std::lock_guard<std::mutex> g(gModel.mu);
	b->length = capacity;
	b->storage.assign(size_t(capacity), 0xee);  // pre-fill non-zero to prove CE overwrote it
	b->contiguous = wantContig;
	const uint32_t gpuPages = uint32_t(capacity >> 12);
	b->pagePas.resize(gpuPages);
	if (wantContig) {
		const uint64_t align = (~mask & 0x3ffffffffULL) + 1ULL;
		gModel.nextPa = (gModel.nextPa + align - 1ULL) & ~(align - 1ULL);
		for (uint32_t i = 0; i < gpuPages; ++i) {
			const uint64_t pa = gModel.nextPa + uint64_t(i) * 4096ULL;
			b->pagePas[i] = pa;
			gModel.physToHost[pa] = b->storage.data() + size_t(i) * 4096;
		}
		gModel.nextPa += capacity;
	} else {
		// Non-contiguous fallback: discontiguous 4 KiB pages (not 128 KiB aligned).
		gModel.nextPa += 4096ULL;
		for (uint32_t i = 0; i < gpuPages; ++i) {
			const uint64_t pa = gModel.nextPa;
			b->pagePas[i] = pa;
			gModel.physToHost[pa] = b->storage.data() + size_t(i) * 4096;
			gModel.nextPa += 8192ULL;
		}
	}
	return b;
}

IOMultiMemoryDescriptor *IOMultiMemoryDescriptor::withDescriptors(IOMemoryDescriptor **descriptors,
								  uint32_t count, IODirection, bool)
{
	auto *m = new TrackedMultiDescriptor();
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		descriptors[i]->retain();
		m->parts.push_back(descriptors[i]);
		total += descriptors[i]->length;
	}
	m->length = total;
	return m;
}

extern "C" {
vfs_context_t vfs_context_kernel(void)
{
	return &gKernelCtx;
}

void *vfs_context_ucred(vfs_context_t)
{
	return nullptr;
}

void *vfs_context_proc(vfs_context_t)
{
	return nullptr;
}

int vnode_open(const char *path, int, int, int, vnode_t *vpp, vfs_context_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	++gModel.openCount;
	if (!gModel.fileExists || gModel.failOpen || std::strcmp(path, TGM_PACK_PATH) != 0)
		return ENOENT;
	*vpp = new switch_pack_vnode{path, false};
	++gModel.openVnodes;
	return 0;
}

int vnode_close(vnode_t vp, int, vfs_context_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	++gModel.closeCount;
	if (vp && !vp->closed) {
		vp->closed = true;
		--gModel.openVnodes;
		delete vp;
	}
	return 0;
}

int vnode_isreg(vnode_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	return gModel.fileRegular ? 1 : 0;
}

int vnode_getattr(vnode_t, struct vnode_attr *vap, vfs_context_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	vap->va_data_size = gModel.fileBytes.size();
	vap->sizeSupported = true;
	return 0;
}

int vn_rdwr(int, vnode_t, caddr_t base, int len, off_t offset, int, int, void *, int *aresid,
	    void *)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	if (offset < 0 || uint64_t(offset) + uint64_t(len) > gModel.fileBytes.size())
		return EIO;
	std::memcpy(base, gModel.fileBytes.data() + offset, size_t(len));
	if (aresid)
		*aresid = 0;
	return 0;
}

int vnode_pagein(vnode_t, upl_t, upl_offset_t, uint64_t f_offset, upl_size_t size, int, int *errorp)
{
	std::unique_lock<std::mutex> g(gModel.mu);
	const int callIdx = int(gModel.pageinCalls++);
	const bool heap = f_offset >= kSwitchGpuPackTierAlign;
	if (!gModel.replaying)
		++gModel.pageinsOutsideReplay;
	if (heap)
		++gModel.heapPageins;
	if (gModel.pauseFirstPagein && callIdx == 0) {
		gModel.firstPageinEntered = true;
		gModel.cv.notify_all();
		gModel.cv.wait(g, [] {
			return gModel.releaseFirstPagein;
		});
	}
	if (heap && gModel.heapPageins == gModel.pauseHeapPagein) {
		gModel.callPaused = true;
		gModel.cv.notify_all();
		gModel.cv.wait(g, [] {
			return gModel.releaseCall;
		});
	}
	if (gModel.failPageinOnCall == callIdx) {
		if (errorp)
			*errorp = 1;
		return EIO;
	}
	// Populate page-cache physical pages for [f_offset, f_offset + size).
	for (uint64_t off = f_offset; off < f_offset + size; off += PAGE_SIZE) {
		if (gModel.pageCachePa.count(off))
			continue;
		gModel.nextPa = (gModel.nextPa + PAGE_SIZE - 1ULL) & ~uint64_t(PAGE_SIZE - 1);
		const uint64_t vmPa = gModel.nextPa;
		gModel.nextPa += PAGE_SIZE;
		gModel.pageCacheStorage.emplace_back(PAGE_SIZE, 0);
		uint8_t *buf = gModel.pageCacheStorage.back().data();
		if (off < gModel.fileBytes.size()) {
			const size_t avail =
				std::min<size_t>(PAGE_SIZE, gModel.fileBytes.size() - off);
			std::memcpy(buf, gModel.fileBytes.data() + off, avail);
		}
		gModel.pageCachePa[off] = vmPa;
		for (uint32_t s = 0; s < PAGE_SIZE / 4096u; ++s)
			gModel.physToHost[vmPa + s * 4096ULL] = buf + s * 4096u;
	}
	if (errorp)
		*errorp = 0;
	return 0;
}

kern_return_t ubc_create_upl_external(vnode_t, off_t f_offset, int bufsize, upl_t *uplp,
				      upl_page_info_t **plp, int uplflags)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	const uint32_t vmPages = uint32_t(bufsize / PAGE_SIZE);
	auto *u = new switch_pack_upl();
	u->offset = uint64_t(f_offset);
	u->size = uint32_t(bufsize);
	u->pl.resize(vmPages);
	u->released.assign(vmPages, false);
	const bool wantAbsent = gModel.injectPartialAbsentOnce || gModel.injectAlwaysAbsentUpl;
	const bool injectAbsent = !(uplflags & UPL_NOBLOCK) && wantAbsent;
	if (gModel.injectPartialAbsentOnce && !(uplflags & UPL_NOBLOCK))
		gModel.injectPartialAbsentOnce = false;
	for (uint32_t i = 0; i < vmPages; ++i) {
		const uint64_t off = uint64_t(f_offset) + uint64_t(i) * PAGE_SIZE;
		auto it = gModel.pageCachePa.find(off);
		if (injectAbsent && i == vmPages / 2) {
			u->pl[i] = {0, false, false};
		} else if (it != gModel.pageCachePa.end()) {
			u->pl[i] = {ppnum_t(it->second >> PAGE_SHIFT), true, true};
		} else {
			u->pl[i] = {0, false, false};
		}
	}
	++gModel.activeUpls;
	if (gModel.activeUpls > gModel.maxConcurrentUpls)
		gModel.maxConcurrentUpls = gModel.activeUpls;
	*uplp = u;
	if (plp)
		*plp = u->pl.data();
	return KERN_SUCCESS;
}

static void markUplReleased(upl_t upl, upl_offset_t offset, upl_size_t size, bool commit)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	const uint32_t first = offset / PAGE_SIZE;
	const uint32_t count = size / PAGE_SIZE;
	for (uint32_t i = first; i < first + count && i < upl->released.size(); ++i)
		upl->released[i] = true;
	bool allReleased = true;
	for (bool r : upl->released)
		if (!r)
			allReleased = false;
	if (commit)
		++gModel.committedUpls;
	else
		++gModel.abortedUpls;
	if (allReleased && !upl->deallocated) {
		upl->deallocated = true;
		--gModel.activeUpls;
		delete upl;
	}
}

kern_return_t ubc_upl_commit(upl_t upl)
{
	markUplReleased(upl, 0, upl->size, true);
	return KERN_SUCCESS;
}

kern_return_t ubc_upl_commit_range(upl_t upl, upl_offset_t offset, upl_size_t size, int)
{
	markUplReleased(upl, offset, size, true);
	return KERN_SUCCESS;
}

kern_return_t ubc_upl_abort_range(upl_t upl, upl_offset_t offset, upl_size_t size, int)
{
	markUplReleased(upl, offset, size, false);
	return KERN_SUCCESS;
}

ppnum_t upl_phys_page(upl_page_info_t *upl, int index)
{
	return upl[index].phys_addr;
}

boolean_t upl_page_present(upl_page_info_t *upl, int index)
{
	return upl[index].present ? 1 : 0;
}

boolean_t upl_valid_page(upl_page_info_t *upl, int index)
{
	return upl[index].valid ? 1 : 0;
}

kern_return_t kernel_thread_start(thread_continue_t cont, void *param, thread_t *out)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	gModel.detachedThreads.emplace_back([cont, param] {
		cont(param, 0);
	});
	*out = nullptr;
	return KERN_SUCCESS;
}

void thread_deallocate(thread_t)
{
}

thread_t current_thread(void)
{
	return nullptr;
}

kern_return_t thread_terminate(thread_t)
{
	return KERN_SUCCESS;
}

int tgm_fence_kernel(unsigned int which, volatile uint32_t **va, uint64_t *page_pa,
		     uint32_t *offset)
{
	if (which >= TGM_FENCE_KERNEL_CHANNELS)
		return -22;
	*va = &gModel.fenceWords[which * 4];
	*page_pa = 0x7f000000ULL;
	*offset = which * 16u;
	return 0;
}

uint32_t tgm_fence_offset(const volatile uint32_t *word)
{
	return 0x100000u + uint32_t(reinterpret_cast<uintptr_t>(word) -
				    reinterpret_cast<uintptr_t>(&gModel.fenceWords[0]));
}

extern "C" void switch_vm_prefetch_replay_thread(void *, int on)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	gModel.replaying = on != 0;
	++gModel.replayMarks;
}

void tgm_acquire_recover_failed(const char *why)
{
	std::unique_lock<std::mutex> g(gModel.mu);
	if (gModel.holdRecovery)
		gModel.recoveredAfterShutdown =
			gModel.cv.wait_for(g, std::chrono::milliseconds(200), [] {
				return gModel.shutdownReturned;
			});
	gModel.recovered.push_back(why);
}

void tgm_cpu_clean_pages(const uint64_t *, uint32_t)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	++gModel.srcCleanCalls;
}

int tgm_mem_new(tgm_vm *, const uint64_t *pages, uint32_t npages, tgm_mem **pmem)
{
	*pmem = new tgm_mem{pages, npages, false};
	return 0;
}

void tgm_mem_del(tgm_mem **pmem)
{
	if (pmem && *pmem) {
		(*pmem)->freed = true;
		delete *pmem;
		*pmem = nullptr;
	}
}

int switch_gpu_vm_map_volatile(tgm_vm *vm, uint64_t va, uint64_t range, tgm_mem *mem, uint64_t off,
			       uint8_t kind, int readOnly, int vol, switch_gpu_mapping **out)
{
	if (!vm || !mem || !out || kind != 0 || off != 0 ||
	    range != uint64_t(mem->npages) * 4096ULL)
		return -22;
	const uint64_t big = switch_gpu_mapping_big_bytes(mem->pages, mem->npages, va, 0, range);
	auto *m = new switch_gpu_mapping{va, range, mem, readOnly, vol, big};
	vm->mappings[va] = m;
	*out = m;
	return 0;
}

void switch_gpu_vm_unmap(tgm_vm *vm, switch_gpu_mapping **mapping)
{
	if (!mapping || !*mapping)
		return;
	if (vm)
		vm->mappings.erase((*mapping)->va);
	delete *mapping;
	*mapping = nullptr;
}
}  // extern "C"

int switch_gpu_cached_copy(const uint64_t *srcPages, const uint64_t *dstPages, uint32_t count)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	const int callIdx = int(gModel.copyCalls++);
	if (gModel.failCopyOnCall == callIdx)
		return gModel.copyError;
	for (uint32_t i = 0; i < count; ++i) {
		auto sit = gModel.physToHost.find(srcPages[i]);
		auto dit = gModel.physToHost.find(dstPages[i]);
		if (sit == gModel.physToHost.end() || dit == gModel.physToHost.end())
			return -22;
		std::memcpy(dit->second, sit->second, 4096);
	}
	return 0;
}

class PackImageTest : public testing::Test
{
protected:
	void SetUp() override
	{
		switch_gpu_pack_image_reset_for_test();
		gModel.reset();
	}

	void TearDown() override
	{
		switch_gpu_pack_image_reset_for_test();
		gModel.joinThreads();
		EXPECT_EQ(gModel.openVnodes, 0);
		EXPECT_EQ(gModel.activeUpls, 0);
		EXPECT_EQ(gModel.liveDescriptors, 0);
		EXPECT_EQ(gModel.liveMaps, 0);
	}
};
