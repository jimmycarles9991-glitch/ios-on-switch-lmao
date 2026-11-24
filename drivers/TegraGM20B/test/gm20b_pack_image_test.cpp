// SPDX-License-Identifier: MIT
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <atomic>
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
	uint32_t warmThreadsStarted = 0;
	int lastWarmPriority = -1;
	int lastIoPolicy = -1;
	uint64_t nextPa = 0x80000000ULL;
	uint64_t nextUserVa = 0x200000000ULL;
	uint64_t clockNs = 1000000ULL;
	std::map<uint64_t, uint8_t *> physToHost;
	std::map<uint64_t, uint64_t> pageCachePa;  // fileOff (16 KiB aligned) -> phys addr
	std::map<std::pair<IOMemoryDescriptor *, task_t>, IOMemoryMap *> taskMaps;
	std::vector<std::vector<uint8_t>> pageCacheStorage;
	std::vector<std::pair<uint64_t, uint64_t>> warmPageins;
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
	bool setPressureOnSecondWarm = false;
	bool warmPressureTriggered = false;
	uint32_t sleepers = 0;
	uint32_t unintSleepers = 0;

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
		warmThreadsStarted = 0;
		lastWarmPriority = -1;
		lastIoPolicy = -1;
		nextPa = 0x80000000ULL;
		nextUserVa = 0x200000000ULL;
		clockNs = 1000000ULL;
		physToHost.clear();
		pageCachePa.clear();
		taskMaps.clear();
		pageCacheStorage.clear();
		warmPageins.clear();
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
		setPressureOnSecondWarm = false;
		warmPressureTriggered = false;
		sleepers = 0;
		unintSleepers = 0;
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
	if (gModel.pauseFirstPagein && callIdx == 0) {
		gModel.firstPageinEntered = true;
		gModel.cv.notify_all();
		gModel.cv.wait(g, [] {
			return gModel.releaseFirstPagein;
		});
	}
	if (gModel.failPageinOnCall == callIdx) {
		if (errorp)
			*errorp = 1;
		return EIO;
	}
	if (gModel.warmThreadsStarted > 0) {
		gModel.warmPageins.emplace_back(f_offset, size);
		if (gModel.setPressureOnSecondWarm && gModel.warmPageins.size() == 2) {
			memorystatus_vm_pressure_level = 1;
			gModel.warmPressureTriggered = true;
		}
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

kern_return_t kernel_thread_start_priority(thread_continue_t cont, void *param, integer_t prio,
					   thread_t *out)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	++gModel.warmThreadsStarted;
	gModel.lastWarmPriority = prio;
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

void throttle_set_thread_io_policy(int policy)
{
	std::lock_guard<std::mutex> g(gModel.mu);
	gModel.lastIoPolicy = policy;
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

TEST_F(PackImageTest, PackHeaderAndAbiLayoutMatchMetalPackH)
{
	static_assert(sizeof(SwitchGpuPackHeader) == sizeof(gm20b::PackHeader));
	static_assert(sizeof(SwitchGpuPackSection) == sizeof(gm20b::PackSection));
	static_assert(kSwitchGpuPackVersion == gm20b::kPackVersion);
	static_assert(uint32_t(kSwitchGpuPackSectionCount) == uint32_t(gm20b::PackSectionCount));
	static_assert(kSwitchGpuPackTierAlign == gm20b::kPackTierAlign);
	static_assert(uint32_t(kSwitchGpuPackPrograms) == uint32_t(gm20b::PackPrograms));
	static_assert(uint32_t(kSwitchGpuPackHeap) == uint32_t(gm20b::PackHeap));
	static_assert(uint32_t(kSwitchGpuPackVariants) == uint32_t(gm20b::PackVariants));
	static_assert(uint32_t(kSwitchGpuPackFastKeys) == uint32_t(gm20b::PackFastKeys));
	static_assert(offsetof(SwitchGpuPackHeader, magic) == offsetof(gm20b::PackHeader, magic));
	static_assert(offsetof(SwitchGpuPackHeader, version) ==
		      offsetof(gm20b::PackHeader, version));
	static_assert(offsetof(SwitchGpuPackHeader, programs) ==
		      offsetof(gm20b::PackHeader, programs));
	static_assert(offsetof(SwitchGpuPackHeader, pipelines) ==
		      offsetof(gm20b::PackHeader, pipelines));
	static_assert(offsetof(SwitchGpuPackHeader, samplers) ==
		      offsetof(gm20b::PackHeader, samplers));
	static_assert(offsetof(SwitchGpuPackHeader, section) ==
		      offsetof(gm20b::PackHeader, section));
	static_assert(offsetof(SwitchGpuPackHeader, stack) == offsetof(gm20b::PackHeader, stack));
	static_assert(offsetof(SwitchGpuPackHeader, chipFacts) ==
		      offsetof(gm20b::PackHeader, chipFacts));
	static_assert(offsetof(SwitchGpuPackHeader, reserved) ==
		      offsetof(gm20b::PackHeader, reserved));
	static_assert(offsetof(SwitchGpuPackHeader, tier0) == offsetof(gm20b::PackHeader, tier0));

	EXPECT_EQ(TGM_PACK_WINDOW_VA, (1ULL << 39) - (4ULL << 30));
	EXPECT_EQ(TGM_PACK_WINDOW_BYTES, 4ULL << 30);
	EXPECT_STREQ(TGM_PACK_PATH, "/System/Library/Extensions/TegraGM20BMetal.pack/pack.bin");
	EXPECT_EQ(TegraGM20BMethodPackImageMap, 41u);
}

TEST_F(PackImageTest, HeaderValidationRefusesMalformedHeaders)
{
	auto bytes = makePackFile(256ULL << 10, 128ULL << 10);
	SwitchGpuPackHeader hdr = {};
	std::memcpy(&hdr, bytes.data(), sizeof(hdr));
	const char *why = nullptr;
	EXPECT_TRUE(switch_gpu_pack_validate_header(&hdr, bytes.size(), &why));
	EXPECT_EQ(why, nullptr);

	auto bad = hdr;
	bad.magic[7] = '2';
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.version = 32;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.section[kSwitchGpuPackHeap].bytes = bytes.size();
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = bad.section[kSwitchGpuPackHeap].bytes + 4096;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = 64ULL << 10;  // not 128 KiB aligned
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	bad = hdr;
	bad.tier0[kSwitchGpuPackHeap] = 0;
	EXPECT_FALSE(switch_gpu_pack_validate_header(&bad, bytes.size(), &why));

	EXPECT_FALSE(switch_gpu_pack_validate_header(&hdr, sizeof(hdr) - 1, &why));
}

TEST_F(PackImageTest, CpuRangeValidationRejectsUnalignedOverflowAndNonZeroTrailing)
{
	TegraGM20BAbiPackCpuRange ranges[TegraGM20BMaxPackCpuRanges] = {};
	uint64_t total = 0;
	ranges[0] = {0x10000000ULL, 0x4000ULL};
	ranges[1] = {0x10010000ULL, 0x8000ULL};
	EXPECT_TRUE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	EXPECT_EQ(total, 0xc000ULL);

	// Non-zero trailing entry past count
	ranges[2] = {0x10020000ULL, 0x4000ULL};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	ranges[2] = {};

	// Unaligned address
	ranges[0].address = 0x10001000ULL;
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 2, PAGE_SIZE, &total));
	ranges[0].address = 0x10000000ULL;

	// Wraparound
	ranges[0] = {UINT64_MAX - PAGE_SIZE + 1ULL, 2ULL * PAGE_SIZE};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 1, PAGE_SIZE, &total));

	// Exceeds 64 MiB cap
	ranges[0] = {0x10000000ULL, kSwitchGpuPackMaxCpuWireBytes + PAGE_SIZE};
	EXPECT_FALSE(switch_gpu_pack_validate_cpu_ranges(ranges, 1, PAGE_SIZE, &total));
}

TEST_F(PackImageTest, SingleBuildDeduplicationConcurrentCallersReceiveSameImageAndBytes)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 128ULL << 10, kSwitchGpuPackVersion,
					0xaabbccddeeff0011ULL);
	gModel.pauseFirstPagein = true;

	SwitchGpuPackImageView v1 = {}, v2 = {}, v3 = {};
	uint64_t w1 = 0, w2 = 0, w3 = 0;
	auto f1 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("cameracaptured", &v1, &w1);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered;
		});
	}
	auto f2 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("backboardd", &v2, &w2);
	});
	auto f3 = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("SpringBoard", &v3, &w3);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.sleepers >= 3;
		});
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}

	ASSERT_EQ(f1.get(), kIOReturnSuccess);
	ASSERT_EQ(f2.get(), kIOReturnSuccess);
	ASSERT_EQ(f3.get(), kIOReturnSuccess);

	EXPECT_TRUE(v1.ready);
	EXPECT_EQ(v1.mem, v2.mem);
	EXPECT_EQ(v1.mem, v3.mem);
	EXPECT_EQ(v1.cpuMemory, v2.cpuMemory);
	EXPECT_EQ(v1.tier0HeapBytes, 256ULL << 10);
	EXPECT_EQ(v1.heapBytes, 384ULL << 10);
	EXPECT_EQ(v1.stack, 0xaabbccddeeff0011ULL);
	EXPECT_EQ(gModel.openCount, 1u);
	EXPECT_EQ(gModel.closeCount, 1u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.maxConcurrentUpls, 1);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_GT(gModel.committedUpls, 0u);
	EXPECT_EQ(gModel.abortedUpls, 0u);
	EXPECT_EQ(gModel.copyCalls, 1u);
	EXPECT_GT(gModel.srcCleanCalls, 0u);
	EXPECT_GT(gModel.dstCleanInvalOps, 0u);
	EXPECT_GT(gModel.dstInvalOps, 0u);

	// Verify copied tier-0 bytes match file byte-for-byte via CE copy (not CPU).
	for (uint32_t p = 0; p < v1.mem->npages; ++p) {
		const uint8_t *dst = gModel.physToHost.at(v1.mem->pages[p]);
		const uint8_t *exp =
			gModel.fileBytes.data() + kSwitchGpuPackTierAlign + size_t(p) * 4096;
		ASSERT_EQ(std::memcmp(dst, exp, 4096), 0) << "mismatch at 4 KiB page " << p;
	}
}

TEST_F(PackImageTest, BadVersionRefusesOnceNeverRetriesAndClosesVnode)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0, 32);
	SwitchGpuPackImageView v1 = {}, v2 = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc1", &v1, nullptr), kIOReturnSuccess);
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc2", &v2, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(v1.ready);
	EXPECT_FALSE(v2.ready);
	EXPECT_EQ(gModel.openCount, 1u);
	EXPECT_EQ(gModel.closeCount, 1u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(switch_gpu_pack_image_refused_count_for_test(), 1u);
	EXPECT_EQ(gModel.logs.size(), 1u);
	EXPECT_THAT(gModel.logs[0], HasSubstr("refused (unsupported version"));
}

TEST_F(PackImageTest, MixedBigPageAndFallbackMapsAtWindowVaInTwoVms)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.failContigChunkIndex = 1;  // chunk 0: 128 KiB contiguous; chunk 1: 4 KiB fallback

	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);

	struct switch_gpu_mapping_run run0 = {}, run1 = {};
	ASSERT_TRUE(switch_gpu_mapping_next(view.mem->pages, view.mem->npages, TGM_PACK_WINDOW_VA,
					    0, view.tier0HeapBytes, &run0));
	EXPECT_EQ(run0.shift, SWITCH_GPU_BIG_SHIFT);
	EXPECT_EQ(run0.bytes, kSwitchGpuPackTierAlign);
	ASSERT_TRUE(switch_gpu_mapping_next(view.mem->pages, view.mem->npages,
					    TGM_PACK_WINDOW_VA + run0.bytes, run0.bytes,
					    view.tier0HeapBytes - run0.bytes, &run1));
	EXPECT_EQ(run1.shift, SWITCH_GPU_SMALL_SHIFT);
	EXPECT_EQ(run1.bytes, kSwitchGpuPackTierAlign);

	tgm_vm vmA = {1, {}}, vmB = {2, {}};
	SwitchGpuPackClient clientA = {}, clientB = {};
	ASSERT_EQ(switch_gpu_vm_map_volatile(&vmA, TGM_PACK_WINDOW_VA, view.tier0HeapBytes,
					     view.mem, 0, 0, 1, 0, &clientA.gpuMapping),
		  0);
	ASSERT_EQ(switch_gpu_vm_map_volatile(&vmB, TGM_PACK_WINDOW_VA, view.tier0HeapBytes,
					     view.mem, 0, 0, 1, 0, &clientB.gpuMapping),
		  0);
	EXPECT_EQ(clientA.gpuMapping->va, TGM_PACK_WINDOW_VA);
	EXPECT_EQ(clientA.gpuMapping->readOnly, 1);
	EXPECT_EQ(clientA.gpuMapping->vol, 0);
	EXPECT_EQ(clientA.gpuMapping->bigBytes, kSwitchGpuPackTierAlign);
	EXPECT_EQ(clientB.gpuMapping->va, TGM_PACK_WINDOW_VA);
	EXPECT_EQ(clientB.gpuMapping->readOnly, 1);

	switch_gpu_pack_client_unmap_gpu(&vmA, &clientA);
	EXPECT_EQ(clientA.gpuMapping, nullptr);
	EXPECT_FALSE(view.mem->freed);
	EXPECT_EQ(vmB.mappings.count(TGM_PACK_WINDOW_VA), 1u);
	switch_gpu_pack_client_unmap_gpu(&vmB, &clientB);
}

TEST_F(PackImageTest, CompositorCpuRangeWiringDropsOnCloseWhileSharedImageSurvives)
{
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);

	switch_pack_task compositorTask = {42};
	SwitchGpuPackClient client = {};

	TegraGM20BAbiPackCpuRange ranges[2] = {
		{0x40000000ULL, 0x10000ULL},
		{0x40100000ULL, 0x20000ULL},
	};
	uint64_t wiredBytes = 0;
	ASSERT_EQ(switch_gpu_pack_client_wire_cpu(&compositorTask, ranges, 2, client.cpuWired,
						  &wiredBytes),
		  kIOReturnSuccess);
	client.cpuWiredCount = 2;
	EXPECT_EQ(wiredBytes, 0x30000ULL);
	EXPECT_EQ(client.cpuWired[0]->preparedDir, kIODirectionOut);
	EXPECT_EQ(client.cpuWired[1]->preparedDir, kIODirectionOut);

	const int sharedRefsBefore = view.cpuMemory->refs.load();
	switch_gpu_pack_client_release_cpu(&client);
	EXPECT_EQ(client.cpuWiredCount, 0u);
	EXPECT_EQ(gModel.liveMaps, 0);
	EXPECT_EQ(view.cpuMemory->refs.load(), sharedRefsBefore);

	// A restarted compositor still gets the same shared image, and partial wire failure rolls back cleanly.
	gModel.failWireRangeIndex = 3;  // 2nd range of next call fails prepare()
	SwitchGpuPackClient restarted = {};
	EXPECT_EQ(switch_gpu_pack_client_wire_cpu(&compositorTask, ranges, 2, restarted.cpuWired,
						  &wiredBytes),
		  kIOReturnBadArgument);
	EXPECT_EQ(restarted.cpuWired[0], nullptr);
	EXPECT_EQ(restarted.cpuWired[1], nullptr);
}

TEST_F(PackImageTest, WiredOrAbsentUplFallsBackToExactReadWithoutRefusingPack)
{
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	gModel.injectAlwaysAbsentUpl = true;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	EXPECT_EQ(gModel.copyCalls, 0u);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	auto *multi = static_cast<IOMultiMemoryDescriptor *>(view.cpuMemory);
	ASSERT_EQ(multi->parts.size(), 1u);
	auto *bmd = static_cast<IOBufferMemoryDescriptor *>(multi->parts[0]);
	const uint8_t *expected = gModel.fileBytes.data() + kSwitchGpuPackTierAlign;
	EXPECT_EQ(std::memcmp(bmd->getBytesNoCopy(), expected, kSwitchGpuPackTierAlign), 0);
}

TEST_F(PackImageTest, PartialAbsentUplRetriesAndCopyFailureCleansUpAllState)
{
	// Case 1: transient partial-absent UPL commits cleanly and succeeds on retry.
	gModel.fileBytes = makePackFile(128ULL << 10, 0);
	gModel.injectPartialAbsentOnce = true;
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &view, nullptr), kIOReturnSuccess);
	EXPECT_TRUE(view.ready);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(gModel.abortedUpls, 0u);
	EXPECT_GE(gModel.committedUpls, 2u);
	EXPECT_EQ(gModel.pageinCalls, 2u);

	// Case 2: CE failure commits the UPL, closes vnode, frees all descriptors, and refuses cleanly.
	switch_gpu_pack_image_reset_for_test();
	gModel.reset();
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.failCopyOnCall = 0;
	SwitchGpuPackImageView failed = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("proc", &failed, nullptr), kIOReturnSuccess);
	EXPECT_FALSE(failed.ready);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(gModel.liveDescriptors, 0);
	EXPECT_EQ(switch_gpu_pack_image_refused_count_for_test(), 1u);
}

TEST_F(PackImageTest, BootCompleteWarmReadThrottlesAndRespectsPressureAndShutdown)
{
	// Full warm pass: tier-1 heap (1.5 MiB) + FastKeys + Programs + Variants.
	gModel.fileBytes = makePackFile(128ULL << 10, 1536ULL << 10);
	SwitchGpuPackImageView view = {};
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);

	switch_gpu_boot_complete();
	switch_gpu_pack_image_wait_warm_for_test();
	EXPECT_EQ(gModel.warmThreadsStarted, 1u);
	EXPECT_EQ(gModel.lastWarmPriority, MAXPRI_THROTTLE);
	EXPECT_EQ(gModel.lastIoPolicy, IOPOL_THROTTLE);
	EXPECT_EQ(gModel.openVnodes, 0);
	// Heap tier 1 (2 chunks: 1 MiB + 512 KiB) + FastKeys + Programs + Variants = 5 pagein calls.
	EXPECT_EQ(gModel.warmPageins.size(), 5u);

	// Memory pressure aborts warm pass early and closes vnode.
	switch_gpu_pack_image_reset_for_test();
	gModel.reset();
	gModel.fileBytes = makePackFile(128ULL << 10, 3ULL << 20);
	gModel.setPressureOnSecondWarm = true;
	ASSERT_EQ(switch_gpu_pack_image_acquire("backboardd", &view, nullptr), kIOReturnSuccess);
	ASSERT_TRUE(view.ready);
	switch_gpu_boot_complete();
	switch_gpu_pack_image_wait_warm_for_test();
	EXPECT_TRUE(gModel.warmPressureTriggered);
	EXPECT_EQ(gModel.warmPageins.size(), 2u);
	EXPECT_EQ(gModel.openVnodes, 0);
}

TEST_F(PackImageTest, ShutdownDrainsActiveBuildClosesVnodeAndWakesWaiter)
{
	gModel.fileBytes = makePackFile(256ULL << 10, 0);
	gModel.pauseFirstPagein = true;

	SwitchGpuPackImageView view = {};
	auto caller = std::async(std::launch::async, [&] {
		return switch_gpu_pack_image_acquire("backboardd", &view, nullptr);
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.firstPageinEntered && gModel.sleepers >= 1;
		});
	}
	auto shuttingDown = std::async(std::launch::async, [] {
		switch_gpu_pack_shutdown();
	});
	{
		std::unique_lock<std::mutex> g(gModel.mu);
		gModel.cv.wait(g, [] {
			return gModel.unintSleepers >= 1;
		});
		gModel.releaseFirstPagein = true;
		gModel.cv.notify_all();
	}
	shuttingDown.get();
	ASSERT_EQ(caller.get(), kIOReturnSuccess);
	EXPECT_FALSE(view.ready);
	EXPECT_EQ(gModel.openVnodes, 0);
	EXPECT_EQ(gModel.activeUpls, 0);
	EXPECT_EQ(gModel.liveDescriptors, 0);
}

TEST_F(PackImageTest, XnuBootCompleteAndShutdownHooksWiredInBothTrees)
{
	const std::string pm10063 =
		SourceText("../../../kernel/xnu-10063.141.1/iokit/Kernel/IOPMrootDomain.cpp");
	const std::string pm12377 =
		SourceText("../../../kernel/xnu-12377.121.6/iokit/Kernel/IOPMrootDomain.cpp");
	for (const std::string &pm : {pm10063, pm12377}) {
		EXPECT_THAT(pm, HasSubstr("extern \"C\" void switch_gpu_boot_complete(void) "
					  "__attribute__((weak));"));
		const std::string bootComplete =
			Between(pm, "case kPowerEventSystemBootCompleted:", "\n\t\tbreak;");
		EXPECT_THAT(bootComplete, HasSubstr("if (switch_gpu_boot_complete)"));
		EXPECT_THAT(bootComplete, HasSubstr("switch_gpu_boot_complete();"));
		EXPECT_EQ(Count(pm, "switch_gpu_boot_complete();"), 1u);
	}
	const std::string shutdown = SourceText("../../../kernel/switch/shutdown/shutdown.c");
	EXPECT_THAT(shutdown,
		    HasSubstr("extern void switch_gpu_pack_shutdown(void) __attribute__((weak));"));
	EXPECT_GE(Count(shutdown, "switch_gpu_pack_shutdown()"), 2u);
}

TEST_F(PackImageTest, PackImageMapDoesNotMapKernelMemoryIntoClientTasks)
{
	const std::string uc = SourceText("../TegraGM20BUserClient.cpp");
	const std::string body = Between(uc, "IOReturn AGXDeviceUserClient::packImageMap(",
					 "\nvoid AGXDeviceUserClient::releaseMappings()");
	EXPECT_THAT(body, Not(HasSubstr("createMappingInTask")));
	EXPECT_THAT(body, HasSubstr("out->cpu_address = 0;"));
}
