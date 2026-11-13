// SPDX-License-Identifier: MIT
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "../SwitchGpuOrdered.h"
#include "drivers/TegraGM20B/SwitchGpuMapping.h"
#include "drivers/TegraGM20B/TegraGM20BTlb.h"
#include "drivers/TegraGM20B/TegraGM20BComp.h"
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using dma_addr_t = uint64_t;
constexpr u8 PAGE_SHIFT = 12;
constexpr u64 PAGE_SIZE = 4096, TGM_USER_VA_START = 0x10000, TGM_KERNEL_VA = 1ull << 39;
constexpr int GFP_KERNEL = 0, NVIF_CLASS_MEM_GF100 = 0, ADDR = 0;

struct mutex {
	std::mutex value;
};

void mutex_lock(mutex *m) __attribute__((no_thread_safety_analysis))
{
	m->value.lock();
}

void mutex_unlock(mutex *m) __attribute__((no_thread_safety_analysis))
{
	m->value.unlock();
}

bool mutex_available(mutex *m) __attribute__((no_thread_safety_analysis))
{
	if (!m->value.try_lock())
		return false;
	m->value.unlock();
	return true;
}

struct list_head {
	std::vector<void *> nodes;
};

void list_add(void *node, list_head *head)
{
	head->nodes.push_back(node);
}

struct ZeroMemory {
	void *value;

	template <class T> operator T *() const
	{
		return static_cast<T *>(value);
	}
};

ZeroMemory kzalloc(size_t bytes, int)
{
	return {calloc(1, bytes)};
}

void kfree(void *ptr)
{
	free(ptr);
}

struct nvif_mem {
	u8 shift;
	u64 bytes;
};

struct nvif_vmm {
	unsigned id;
};

struct nvif_vma {
	u64 addr;
};

struct nvif_mem_ram_v0 {
	unsigned version;
	dma_addr_t *dma;
};

struct gf100_vmm_map_v0 {
	unsigned version, vol, ro, priv, kind;
};

struct switch_gpu_memory_view {
	switch_gpu_memory_view *next;
	nvif_mem mem;
	u64 offset, bytes;
};

struct tgm_mem {
	nvif_mem mem;
	u64 bytes;
	u8 page;
	const u64 *pages;
	u32 npages;
	switch_gpu_memory_view *big_views;
	bool big_views_ready;
};

struct switch_gpu_mapping_entry {
	u64 va, offset, bytes;
	u8 shift;
	u8 pending;
};

struct switch_gpu_mapping {
	u32 count;
	switch_gpu_mapping_entry entries[];
};

struct tgm_kmap {
	void *head;
	nvif_vma vma;
};

struct tgm_vm {
	nvif_vmm vmm;
	mutex map_lock;
	list_head kmaps;
	tgm_tlb_space tlb;
	const void *owner;
};

struct Mmu {
	u8 kind[256];
};

struct Device {
	void *ltc;
} device;

unsigned flushes;

void nvkm_ltc_flush(void *)
{
	++flushes;
}

struct {
	Device *device = &::device;
	Mmu mmu;
	unsigned mem_type;
} tgm_gpu;

bool nvif_mmu_kind_valid(Mmu *, u8)
{
	return true;
}

int stats_on()
{
	return 0;
}

void tgm_stats_map(uint64_t, uint64_t, uint64_t)
{
}

int l2_on()
{
	return 1;
}

SwitchGpuOrdered ordered;
thread_local unsigned gateDepth;
thread_local int identity;

uintptr_t owner()
{
	return reinterpret_cast<uintptr_t>(&identity);
}

unsigned nvxnu_gpu_ordered_begin()
{
	return ordered.enter(owner());
}

void nvxnu_gpu_ordered_end(unsigned scope)
{
	ordered.leave(scope);
}

std::mutex gateMutex, callsMutex;
std::condition_variable gateCv;
bool gateShut;
unsigned gateInside;
std::atomic<unsigned> gateWaiting;

void tgm_gate_enter()
{
	std::unique_lock held(gateMutex);
	++gateWaiting;
	gateCv.wait(held, [] {
		return !gateShut;
	});
	--gateWaiting;
	++gateInside;
	++gateDepth;
}

void gate_leave()
{
	std::lock_guard held(gateMutex);
	--gateInside;
	--gateDepth;
	gateCv.notify_all();
}

struct Call {
	unsigned vm;
	char operation;
	u64 va, bytes, offset;
	u8 shift;
	bool deferred;
	nvif_mem *memory;
};

std::vector<Call> calls;
std::function<void(unsigned)> mapHook;
int failGet = -1, failMap = -1, invalidations, constructors;
std::atomic<int> getCount, mapCount;

void record(nvif_vmm *vm, char operation, u64 va, u64 bytes, u8 shift, u64 offset = 0,
	    nvif_mem *memory = nullptr)
{
	EXPECT_GT(gateDepth, 0u);
	std::lock_guard held(callsMutex);
	calls.push_back(
		{vm->id, operation, va, bytes, offset, shift, ordered.active(owner()), memory});
}

int nvif_vmm_raw_get(nvif_vmm *vm, u64 va, u64 bytes, u8 shift)
{
	record(vm, 'g', va, bytes, shift);
	return getCount++ == failGet ? -12 : 0;
}

int nvif_vmm_raw_map(nvif_vmm *vm, u64 va, u64 bytes, u8 shift, void *, size_t, nvif_mem *memory,
		     u64 offset)
{
	record(vm, 'm', va, bytes, shift, offset, memory);
	EXPECT_GE(memory->shift, shift);
	if (mapHook)
		mapHook(vm->id);
	return mapCount++ == failMap ? -5 : 0;
}

int nvif_vmm_raw_unmap(nvif_vmm *vm, u64 va, u64 bytes, u8 shift, bool)
{
	record(vm, 'u', va, bytes, shift);
	EXPECT_FALSE(ordered.active(owner()));
	return 0;
}

void nvif_vmm_raw_put(nvif_vmm *vm, u64 va, u64 bytes, u8 shift)
{
	record(vm, 'p', va, bytes, shift);
	EXPECT_FALSE(ordered.active(owner()));
}

void mmu_invalidate_all()
{
	EXPECT_GT(gateDepth, 0u);
	EXPECT_FALSE(ordered.active(owner()));
	++invalidations;
}

int nvif_mem_ctor_type(Mmu *, const char *, int, unsigned, u8 shift, u64 bytes, nvif_mem_ram_v0 *,
		       size_t, nvif_mem *mem)
{
	++constructors;
	*mem = {shift, bytes};
	return 0;
}

int nvif_vmm_get(nvif_vmm *vm, int, bool, u8 shift, unsigned, u64 bytes, nvif_vma *vma)
{
	vma->addr = TGM_KERNEL_VA;
	return nvif_vmm_raw_get(vm, vma->addr, bytes, shift);
}

int nvif_vmm_map(nvif_vmm *vm, u64 va, u64 bytes, void *args, size_t size, nvif_mem *mem,
		 u64 offset)
{
	return nvif_vmm_raw_map(vm, va, bytes, 12, args, size, mem, offset);
}

void nvif_vmm_put(nvif_vmm *vm, nvif_vma *vma)
{
	nvif_vmm_raw_put(vm, vma->addr, 4096, 12);
}

static int tgm_vm_kmap_raw(tgm_vm *, tgm_mem *, uint64_t, int, uint64_t *);
#include "../SwitchGpuVmMap.h"
