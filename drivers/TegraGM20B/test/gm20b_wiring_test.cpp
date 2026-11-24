// SPDX-License-Identifier: MIT
// TegraGM20BSharedWiring.cpp on the host: one wiring per task and range, shared while the task still maps it, released
// by its last import.
#include <set>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BSharedWiring.h"

std::map<std::pair<task_t, uint64_t>, uint64_t> &mappings()
{
	static std::map<std::pair<task_t, uint64_t>, uint64_t> m;
	return m;
}

namespace
{
std::set<tgm_mem *> &deletedMems()
{
	static std::set<tgm_mem *> d;
	return d;
}
}  // namespace

extern "C" {
uint64_t pmap_find_pa(pmap_t pmap, uint64_t va)
{
	auto it = mappings().find({reinterpret_cast<task_t>(pmap), va});
	return it == mappings().end() ? 0 : it->second;
}

pmap_t get_task_pmap(task_t task)
{
	return reinterpret_cast<pmap_t>(task);
}

void tgm_mem_del(struct tgm_mem **pmem)
{
	if (*pmem)
		deletedMems().insert(*pmem);
	*pmem = nullptr;
}
}

namespace
{
constexpr uint64_t kAddress = 0x100000000, kBytes = 4 * 4096;
task_t const kTask = reinterpret_cast<task_t>(0x1000);

SharedWiring *make(IOMemoryDescriptor *wired, uint64_t firstPa, uint32_t surfaceId, tgm_mem *mem)
{
	auto *w = IONew(SharedWiring, 1);
	auto *pas = IONew(uint64_t, 4);
	for (uint32_t i = 0; i < 4; i++)
		pas[i] = firstPa + i * 0x10000;
	*w = {nullptr, kTask, kAddress, kBytes, surfaceId, 0, wired, pas, 4, mem, false};
	return w;
}

void mapTask(uint64_t firstPa)
{
	mappings()[{kTask, kAddress}] = firstPa;
	mappings()[{kTask, kAddress + kBytes - 4096}] = firstPa + 3 * 0x10000;
}

TEST(SharedWiring, OneWiringPerRangeUntilTheLastImportGoes)
{
	IOMemoryDescriptor wired;
	auto *mem = reinterpret_cast<tgm_mem *>(0x2000);
	mapTask(0x80000000);
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes, 7), nullptr);
	SharedWiring *first = sharedWiringPublish(make(&wired, 0x80000000, 7, mem));
	SharedWiring *twin = sharedWiringFind(kTask, kAddress, kBytes, 7);
	EXPECT_EQ(twin, first);
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes, 8), nullptr);
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes + 4096, 7), nullptr);
	sharedWiringPut(first);
	EXPECT_EQ(wired.completed, 0);
	EXPECT_FALSE(deletedMems().count(mem));
	sharedWiringPut(twin);
	EXPECT_EQ(wired.completed, 1);
	EXPECT_EQ(wired.released, 1);
	EXPECT_TRUE(deletedMems().count(mem));
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes, 7), nullptr);
}

TEST(SharedWiring, ARangeTheTaskMapsAfreshIsNotShared)
{
	IOMemoryDescriptor wired;
	mapTask(0x90000000);
	SharedWiring *first = sharedWiringPublish(
		make(&wired, 0x90000000, 9, reinterpret_cast<tgm_mem *>(0x3000)));
	mapTask(0xa0000000);
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes, 9), nullptr);
	mappings().erase({kTask, kAddress});
	EXPECT_EQ(sharedWiringFind(kTask, kAddress, kBytes, 9), nullptr);
	sharedWiringPut(first);
	EXPECT_EQ(wired.completed, 1);
}

TEST(SharedWiring, ARacingPublishOfOtherPagesKeepsItsOwn)
{
	IOMemoryDescriptor first, second;
	mapTask(0xc0000000);
	SharedWiring *a = sharedWiringPublish(
		make(&first, 0xc0000000, 13, reinterpret_cast<tgm_mem *>(0x6000)));
	SharedWiring *other = make(&second, 0xc0000000, 13, reinterpret_cast<tgm_mem *>(0x7000));
	other->pas[2] = 0xd0000000;
	SharedWiring *b = sharedWiringPublish(other);
	EXPECT_NE(a, b);
	EXPECT_EQ(second.completed, 0);
	sharedWiringPut(b);
	sharedWiringPut(a);
	EXPECT_EQ(first.completed, 1);
	EXPECT_EQ(second.completed, 1);
}

TEST(SharedWiring, ARacingPublishTakesTheFirstAndFreesItsOwn)
{
	IOMemoryDescriptor first, second;
	auto *kept = reinterpret_cast<tgm_mem *>(0x4000),
	     *dropped = reinterpret_cast<tgm_mem *>(0x5000);
	mapTask(0xb0000000);
	SharedWiring *a = sharedWiringPublish(make(&first, 0xb0000000, 11, kept));
	SharedWiring *b = sharedWiringPublish(make(&second, 0xb0000000, 11, dropped));
	EXPECT_EQ(a, b);
	EXPECT_EQ(second.completed, 1);
	EXPECT_TRUE(deletedMems().count(dropped));
	EXPECT_FALSE(deletedMems().count(kept));
	sharedWiringPut(a);
	sharedWiringPut(b);
	EXPECT_EQ(first.completed, 1);
}

struct ImportBoModel {
	SharedWiring *wiring = nullptr;
	IOMemoryDescriptor *wired = nullptr;
	uint64_t *pas = nullptr;
	uint32_t npages = 4;
	tgm_mem *mem = nullptr;
	bool backingMemory = false;
	bool ownsMem = false;
	bool readOnly = false;
};

void adoptModel(ImportBoModel *bo, SharedWiring *w)
{
	bo->wiring = w;
	bo->wired = w->wired;
	bo->pas = w->pas;
	bo->mem = w->mem;
	bo->readOnly = w->readOnly;
	bo->ownsMem = false;
}

void publishModel(ImportBoModel *bo, uint32_t surfaceId)
{
	auto *made = IONew(SharedWiring, 1);
	tgm_mem *const backingMem = bo->backingMemory ? bo->mem : nullptr;
	tgm_mem *const sharedMem = bo->ownsMem ? bo->mem : nullptr;
	if (sharedMem)
		bo->ownsMem = false;
	*made = {};
	made->task = kTask;
	made->address = kAddress;
	made->bytes = kBytes;
	made->surfaceId = surfaceId;
	made->wired = bo->wired;
	made->pas = bo->pas;
	made->npages = bo->npages;
	made->mem = sharedMem;
	made->readOnly = bo->readOnly;
	adoptModel(bo, sharedWiringPublish(made));
	if (backingMem) {
		bo->mem = backingMem;
		bo->backingMemory = true;
		bo->ownsMem = false;
	}
}

void releaseModel(ImportBoModel *bo)
{
	if (!bo->ownsMem)
		bo->mem = nullptr;
	tgm_mem_del(&bo->mem);
	if (bo->wiring) {
		sharedWiringPut(bo->wiring);
		bo->wiring = nullptr;
		bo->pas = nullptr;
	}
}

TEST(SharedWiring, SharedWiringHitWithFallbackMemFreesFallbackOnRelease)
{
	deletedMems().clear();
	IOMemoryDescriptor first;
	auto *canonical = reinterpret_cast<tgm_mem *>(0x8100);
	auto *fallback = reinterpret_cast<tgm_mem *>(0x8200);
	mapTask(0xd0000000);
	SharedWiring *w = sharedWiringPublish(make(&first, 0xd0000000, 21, nullptr));
	ImportBoModel bo1{};
	adoptModel(&bo1, w);
	bo1.mem = canonical;
	bo1.backingMemory = true;

	ImportBoModel bo2{};
	SharedWiring *hit = sharedWiringFind(kTask, kAddress, kBytes, 21);
	ASSERT_EQ(hit, w);
	adoptModel(&bo2, hit);
	EXPECT_EQ(bo2.mem, nullptr);
	bo2.mem = fallback;
	bo2.ownsMem = true;

	releaseModel(&bo2);
	EXPECT_TRUE(deletedMems().count(fallback));
	EXPECT_FALSE(deletedMems().count(canonical));
	EXPECT_EQ(first.completed, 0);

	releaseModel(&bo1);
	EXPECT_FALSE(deletedMems().count(canonical));
	EXPECT_EQ(first.completed, 1);
}

TEST(SharedWiring, RacingPublishKeepsCanonicalBackingMemAndRecoversOnNullWinner)
{
	deletedMems().clear();
	IOMemoryDescriptor w1, w2;
	auto *memA = reinterpret_cast<tgm_mem *>(0x9100);
	auto *canonical = reinterpret_cast<tgm_mem *>(0x9200);
	mapTask(0xe0000000);
	SharedWiring *existing = sharedWiringPublish(make(&w1, 0xe0000000, 22, memA));

	ImportBoModel boB{};
	boB.wired = &w2;
	boB.pas = IONew(uint64_t, 4);
	for (uint32_t i = 0; i < 4; i++)
		boB.pas[i] = 0xe0000000 + i * 0x10000;
	boB.mem = canonical;
	boB.backingMemory = true;
	publishModel(&boB, 22);
	EXPECT_EQ(boB.wiring, existing);
	EXPECT_EQ(boB.mem, canonical);
	EXPECT_TRUE(boB.backingMemory);
	EXPECT_FALSE(boB.ownsMem);
	EXPECT_EQ(w2.completed, 1);
	EXPECT_FALSE(deletedMems().count(memA));
	releaseModel(&boB);
	EXPECT_FALSE(deletedMems().count(memA));
	sharedWiringPut(existing);
	EXPECT_TRUE(deletedMems().count(memA));
	EXPECT_FALSE(deletedMems().count(canonical));

	deletedMems().clear();
	IOMemoryDescriptor w3, w4;
	auto *fallbackB = reinterpret_cast<tgm_mem *>(0x9300);
	auto *recovered = reinterpret_cast<tgm_mem *>(0x9400);
	mapTask(0xf0000000);
	SharedWiring *nullWinner = sharedWiringPublish(make(&w3, 0xf0000000, 23, nullptr));
	ImportBoModel boC{};
	boC.wired = &w4;
	boC.pas = IONew(uint64_t, 4);
	for (uint32_t i = 0; i < 4; i++)
		boC.pas[i] = 0xf0000000 + i * 0x10000;
	boC.mem = fallbackB;
	boC.ownsMem = true;
	publishModel(&boC, 23);
	EXPECT_EQ(boC.wiring, nullWinner);
	EXPECT_EQ(boC.mem, nullptr);
	EXPECT_FALSE(boC.ownsMem);
	EXPECT_TRUE(deletedMems().count(fallbackB));
	boC.mem = recovered;
	boC.ownsMem = true;
	releaseModel(&boC);
	EXPECT_TRUE(deletedMems().count(recovered));
	sharedWiringPut(nullWinner);
	EXPECT_EQ(w3.completed, 1);
}
}  // namespace
