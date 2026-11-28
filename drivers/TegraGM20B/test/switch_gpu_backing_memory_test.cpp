// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuBackingMemory.h"
#include <functional>
#include <vector>

namespace
{
struct Memory {
	const uint64_t *pages;
	uint32_t count;
	uint64_t first;
};

std::atomic<unsigned> created, destroyed;
uint32_t memoryFlags;
int creationError;
std::function<void()> creationHook;

int createMemory(const uint64_t *pages, uint32_t count, void **out, uint32_t *flags)
{
	++created;
	*flags = memoryFlags;
	*out = new Memory{pages, count, pages[0]};
	if (creationHook)
		creationHook();
	return creationError;
}

void destroyMemory(void *object)
{
	auto *memory = static_cast<Memory *>(object);
	EXPECT_EQ(memory->pages[0], memory->first);
	++destroyed;
	delete memory;
}

void destroyOther(void *object)
{
	destroyMemory(object);
}

void destroyMapping(void *object)
{
	EXPECT_TRUE(ioLocksHeld().empty());
	destroyMemory(object);
}

class BackingMemory : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		created = destroyed = 0;
		memoryFlags = 0;
		creationError = 0;
		creationHook = {};
	}

	void TearDown() override
	{
		creationHook = {};
		Backing::TearDown();
		EXPECT_EQ(created.load(), destroyed.load());
	}

	int acquire(switch_gpu_backing *b, uint32_t count, void **object)
	{
		return switch_gpu_backing_memory_acquire(b, count, createMemory, destroyMemory,
							 object);
	}
};
}

TEST_F(BackingMemory, AliasesKeepOneObjectAndItsStablePageVector)
{
	auto *b = backing(4);
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(b, &info);
	auto *alias = switch_gpu_backing_register(info.pages, info.page_count, 0, nullptr, nullptr);
	ASSERT_EQ(alias, b);
	void *first = nullptr, *second = nullptr;
	ASSERT_EQ(acquire(b, 4, &first), 0);
	ASSERT_EQ(acquire(alias, 2, &second), 0);
	EXPECT_EQ(first, second);
	EXPECT_EQ(created, 1u);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 0u);
	EXPECT_EQ(static_cast<Memory *>(second)->pages, info.pages);
	switch_gpu_backing_release(alias);
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 1u);
}

TEST_F(BackingMemory, FinalHandleReleaseKeepsTagsUntilExactProducerRetires)
{
	auto *b = backing();
	void *object = nullptr;
	ASSERT_EQ(acquire(b, 1, &object), 0);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, token(701, 8)), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 0u);
	const auto completed = token(701, 8);
	timelines[key(&completed)].completed = 8;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 1u);
}

TEST_F(BackingMemory, ObjectIsDestroyedBeforePinnedPages)
{
	const uint64_t page = next_page += 4096;
	bool released = false;
	auto *b = switch_gpu_backing_register(&page, 1, 0, &released, [](void *owner) {
		EXPECT_EQ(destroyed, 1u);
		*static_cast<bool *>(owner) = true;
	});
	void *object = nullptr;
	ASSERT_EQ(acquire(b, 1, &object), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_TRUE(released);
}

TEST_F(BackingMemory, CompressionExtentCannotGrowWhileAliasesRemain)
{
	const std::array<uint64_t, 2> pages{next_page += 4096, next_page += 4096};
	auto *b = switch_gpu_backing_register(pages.data(), 1, 0, nullptr, nullptr);
	memoryFlags = SWITCH_GPU_BACKING_MEMORY_FIXED;
	void *object = nullptr;
	ASSERT_EQ(acquire(b, 1, &object), 0);
	EXPECT_EQ(switch_gpu_backing_register(pages.data(), 2, 0, nullptr, nullptr), nullptr);
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(b, &info);
	EXPECT_EQ(info.page_count, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, UncompressedGrowthKeepsOldVectorAndRejectsOversizedObjectReuse)
{
	const std::array<uint64_t, 2> pages{next_page += 4096, next_page += 4096};
	auto *b = switch_gpu_backing_register(pages.data(), 1, 0, nullptr, nullptr);
	void *object = nullptr;
	ASSERT_EQ(acquire(b, 1, &object), 0);
	auto *grown = switch_gpu_backing_register(pages.data(), 2, 0, nullptr, nullptr);
	ASSERT_EQ(grown, b);
	EXPECT_EQ(static_cast<Memory *>(object)->pages[0], pages[0]);
	void *larger = nullptr;
	EXPECT_EQ(acquire(grown, 2, &larger), -95);
	EXPECT_EQ(larger, nullptr);
	switch_gpu_backing_release(grown);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, FailedConstructionReleasesOnlyCandidateAndCanRetry)
{
	auto *b = backing();
	creationError = -12;
	void *object = nullptr;
	EXPECT_EQ(acquire(b, 1, &object), -12);
	EXPECT_EQ(object, nullptr);
	EXPECT_EQ(destroyed, 1u);
	creationError = 0;
	ASSERT_EQ(acquire(b, 1, &object), 0);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, ConcurrentFactoriesPublishOneObjectOutsideOwnershipLock)
{
	auto *b = backing();
	std::promise<void> both;
	auto ready = both.get_future().share();
	std::atomic<unsigned> entered = 0;
	creationHook = [&] {
		switch_gpu_backing_info info{};
		switch_gpu_backing_describe(b, &info);
		EXPECT_EQ(info.page_count, 1u);
		if (++entered == 2)
			both.set_value();
		ready.wait();
	};
	void *one = nullptr, *two = nullptr;
	auto first = std::async(std::launch::async, [&] {
		return acquire(b, 1, &one);
	});
	auto second = std::async(std::launch::async, [&] {
		return acquire(b, 1, &two);
	});
	ASSERT_EQ(first.get(), 0);
	ASSERT_EQ(second.get(), 0);
	EXPECT_EQ(one, two);
	EXPECT_EQ(created, 2u);
	EXPECT_EQ(destroyed, 1u);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, ConcurrentGrowthRejectsStaleCandidateBeforePublication)
{
	const std::array<uint64_t, 2> pages{next_page += 4096, next_page += 4096};
	auto *b = switch_gpu_backing_register(pages.data(), 1, 0, nullptr, nullptr);
	creationHook = [&] {
		auto *grown = switch_gpu_backing_register(pages.data(), 2, 0, nullptr, nullptr);
		EXPECT_EQ(grown, b);
		switch_gpu_backing_release(grown);
	};
	void *object = nullptr;
	EXPECT_EQ(acquire(b, 1, &object), SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(object, nullptr);
	EXPECT_EQ(destroyed, 1u);
	creationHook = {};
	ASSERT_EQ(acquire(b, 2, &object), 0);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, IncompatibleOwnerAndCompressedPrefixFailWithoutReplacingObject)
{
	auto *b = backing(2);
	memoryFlags = SWITCH_GPU_BACKING_MEMORY_FIXED;
	void *object = nullptr;
	EXPECT_EQ(acquire(b, 1, &object), -95);
	EXPECT_EQ(object, nullptr);
	ASSERT_EQ(acquire(b, 2, &object), 0);
	void *other = nullptr;
	EXPECT_EQ(switch_gpu_backing_memory_acquire(b, 2, createMemory, destroyOther, &other), -95);
	EXPECT_EQ(other, nullptr);
	switch_gpu_backing_release(b);
}

namespace
{
std::vector<uint64_t> forgotten;

void forgetVic(uint64_t allocation)
{
	forgotten.push_back(allocation);
}
}

// An engine's map cache holds no reference on what it maps (SwitchGpuBackingMaps.h): the backing's destruction tells
// every engine that noted a mapping of it, after its last job and before its pages go back.
TEST_F(BackingMemory, EnginesThatMappedABackingForgetItBeforeItsPagesGoBack)
{
	ASSERT_EQ(switch_gpu_backing_register_forget(SWITCH_GPU_ENGINE_VIC, forgetVic), 0);
	EXPECT_EQ(switch_gpu_backing_register_forget(SWITCH_GPU_ENGINE_VIC, forgetVic), 0);
	EXPECT_EQ(switch_gpu_backing_register_forget(SWITCH_GPU_ENGINE_VIC, nullptr), -22);
	forgotten.clear();
	const uint64_t page = next_page += 4096;
	bool released = false;
	auto *b = switch_gpu_backing_register(&page, 1, 0, &released, [](void *owner) {
		EXPECT_EQ(forgotten.size(), 1u);
		*static_cast<bool *>(owner) = true;
	});
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(b, &info);
	switch_gpu_backing_note_mapped(b, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_READ, token(702, 4)), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_TRUE(forgotten.empty()) << "a job still reads it";
	const auto completed = token(702, 4);
	timelines[key(&completed)].completed = 4;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	ASSERT_EQ(forgotten.size(), 1u);
	EXPECT_EQ(forgotten[0], info.allocation_id);
	EXPECT_TRUE(released);

	auto *unmapped = backing();
	switch_gpu_backing_release(unmapped);
	switch_gpu_backing_retire();
	EXPECT_EQ(forgotten.size(), 1u) << "a backing no engine mapped is forgotten by none";
}

TEST_F(BackingMemory, EngineMappingSurvivesAliasesAndRetiresBeforeItsPages)
{
	const uint64_t page = next_page += 4096;
	bool released = false;
	auto *b = switch_gpu_backing_register(&page, 1, 0, &released, [](void *owner) {
		EXPECT_EQ(destroyed, 1u);
		*static_cast<bool *>(owner) = true;
	});
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(b, &info);
	void *memory = nullptr;
	uint32_t flags = 0;
	ASSERT_EQ(createMemory(info.pages, 1, &memory, &flags), 0);
	ASSERT_EQ(switch_gpu_backing_mapping_install(b, SWITCH_GPU_ENGINE_NVJPG,
						     info.allocation_generation, memory,
						     destroyMapping),
		  0);
	auto *alias = switch_gpu_backing_register(&page, 1, 0, nullptr, nullptr);
	ASSERT_EQ(alias, b);
	EXPECT_EQ(switch_gpu_backing_mapping_context(alias, SWITCH_GPU_ENGINE_NVJPG), memory);
	EXPECT_EQ(switch_gpu_backing_mapping_install(alias, SWITCH_GPU_ENGINE_NVJPG,
						     info.allocation_generation, memory,
						     destroyMapping),
		  -16);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, token(701, 8)), 0);
	switch_gpu_backing_release(alias);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 0u);
	EXPECT_FALSE(released);
	const auto completed = token(701, 8);
	timelines[key(&completed)].completed = 8;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 1u);
	EXPECT_TRUE(released);
}

TEST_F(BackingMemory, InstalledEngineMappingSurvivesPrefixPreservingGrowth)
{
	const std::array<uint64_t, 2> pages{next_page += 4096, next_page += 4096};
	bool firstOwnerReleased = false, secondOwnerReleased = false;
	auto *b = switch_gpu_backing_register(pages.data(), 1, 77, &firstOwnerReleased,
					      [](void *owner) {
						      *static_cast<bool *>(owner) = true;
					      });
	ASSERT_NE(b, nullptr);
	switch_gpu_backing_info before{};
	switch_gpu_backing_describe(b, &before);
	void *memory = nullptr;
	uint32_t flags = 0;
	ASSERT_EQ(createMemory(before.pages, 1, &memory, &flags), 0);
	EXPECT_EQ(switch_gpu_backing_mapping_install(b, SWITCH_GPU_ENGINE_NVJPG,
						     before.allocation_generation + 1, memory,
						     destroyMapping),
		  SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(switch_gpu_backing_mapping_context(b, SWITCH_GPU_ENGINE_NVJPG), nullptr);
	ASSERT_EQ(switch_gpu_backing_mapping_install(b, SWITCH_GPU_ENGINE_NVJPG,
						     before.allocation_generation, memory,
						     destroyMapping),
		  0);
	EXPECT_EQ(switch_gpu_backing_register(pages.data(), 2, 77, nullptr, nullptr), nullptr);
	const std::array<uint64_t, 2> mismatch{pages[0], next_page += 4096};
	auto *grown = switch_gpu_backing_register(pages.data(), 2, 77, &secondOwnerReleased,
						  [](void *owner) {
							  EXPECT_EQ(destroyed, 1u);
							  *static_cast<bool *>(owner) = true;
						  });
	ASSERT_EQ(grown, b);
	EXPECT_TRUE(firstOwnerReleased);
	EXPECT_FALSE(secondOwnerReleased);
	switch_gpu_backing_info after{};
	switch_gpu_backing_describe(grown, &after);
	EXPECT_EQ(after.page_count, 2u);
	EXPECT_NE(after.allocation_generation, before.allocation_generation);
	EXPECT_EQ(before.pages[0], pages[0]);
	EXPECT_EQ(switch_gpu_backing_mapping_context(grown, SWITCH_GPU_ENGINE_NVJPG), memory);
	EXPECT_EQ(switch_gpu_backing_register(mismatch.data(), 2, 77, &secondOwnerReleased,
					      [](void *) {}),
		  nullptr);
	switch_gpu_backing_release(grown);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(destroyed, 1u);
	EXPECT_TRUE(secondOwnerReleased);
}

TEST_F(BackingMemory, ExistingLookupReturnsCanonicalObjectWithoutCreating)
{
	auto *b = backing(4);
	void *existing = nullptr;
	EXPECT_EQ(switch_gpu_backing_memory_existing(b, 4, destroyMemory, &existing), -2);
	EXPECT_EQ(existing, nullptr);
	EXPECT_EQ(created, 0u);

	void *canonical = nullptr;
	ASSERT_EQ(acquire(b, 4, &canonical), 0);
	ASSERT_EQ(switch_gpu_backing_memory_existing(b, 2, destroyMemory, &existing), 0);
	EXPECT_EQ(existing, canonical);
	EXPECT_EQ(created, 1u);

	EXPECT_EQ(switch_gpu_backing_memory_existing(b, 5, destroyMemory, &existing), -95);
	EXPECT_EQ(existing, nullptr);
	EXPECT_EQ(switch_gpu_backing_memory_existing(b, 4, destroyOther, &existing), -95);
	EXPECT_EQ(existing, nullptr);

	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, DirectPaLatchRefusesSmmuAndFallsBackToDirect)
{
	auto *b = backing(16);
	void *existing = nullptr;
	EXPECT_EQ(switch_gpu_backing_memory_existing_or_direct(b, 16, destroyMemory, &existing),
		  -2);
	EXPECT_EQ(existing, nullptr);

	static std::atomic<unsigned> smmuCalls{0};
	smmuCalls = 0;
	auto createSmmu = [](const uint64_t *pages, uint32_t count, void **out, uint32_t *flags) {
		++smmuCalls;
		++created;
		*flags = SWITCH_GPU_BACKING_MEMORY_FIXED | SWITCH_GPU_BACKING_MEMORY_SMMU;
		*out = new Memory{pages, count, pages[0]};
		return 0;
	};
	void *object = nullptr;
	ASSERT_EQ(switch_gpu_backing_memory_acquire_smmu(b, 16, createSmmu, createMemory,
							 destroyMemory, &object),
		  0);
	EXPECT_NE(object, nullptr);
	EXPECT_EQ(smmuCalls.load(), 0u);
	EXPECT_EQ(created.load(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, ConcurrentDirectPaLatchDiscardsUnpublishedSmmuCandidate)
{
	auto *b = backing(16);
	static std::atomic<unsigned> smmuCalls{0};
	smmuCalls = 0;
	creationHook = [&] {
		void *existing = nullptr;
		EXPECT_EQ(switch_gpu_backing_memory_existing_or_direct(b, 16, destroyMemory,
								       &existing),
			  -2);
		EXPECT_EQ(existing, nullptr);
		creationHook = {};
	};
	auto createSmmu = [](const uint64_t *pages, uint32_t count, void **out, uint32_t *flags) {
		++smmuCalls;
		++created;
		*flags = SWITCH_GPU_BACKING_MEMORY_FIXED | SWITCH_GPU_BACKING_MEMORY_SMMU;
		*out = new Memory{pages, count, pages[0]};
		if (creationHook)
			creationHook();
		return 0;
	};
	void *object = nullptr;
	ASSERT_EQ(switch_gpu_backing_memory_acquire_smmu(b, 16, createSmmu, createMemory,
							 destroyMemory, &object),
		  0);
	EXPECT_NE(object, nullptr);
	EXPECT_EQ(smmuCalls.load(), 1u);
	EXPECT_EQ(created.load(), 2u);
	EXPECT_EQ(destroyed.load(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(BackingMemory, UncompressedSmmuMemoryFixesExtentAndPreventsAliasedGrowth)
{
	std::vector<uint64_t> pages(32);
	for (uint32_t i = 0; i < 32; ++i)
		pages[i] = next_page += 4096;
	auto *b = switch_gpu_backing_register(pages.data(), 16, 0, nullptr, nullptr);
	ASSERT_NE(b, nullptr);
	auto createUncompressedSmmu = [](const uint64_t *p, uint32_t count, void **out,
					 uint32_t *flags) {
		++created;
		*flags = SWITCH_GPU_BACKING_MEMORY_SMMU;
		*out = new Memory{p, count, p[0]};
		return 0;
	};
	void *object = nullptr;
	ASSERT_EQ(switch_gpu_backing_memory_acquire_smmu(b, 16, createUncompressedSmmu,
							 createMemory, destroyMemory, &object),
		  0);
	ASSERT_NE(object, nullptr);
	EXPECT_EQ(switch_gpu_backing_register(pages.data(), 32, 0, nullptr, nullptr), nullptr)
		<< "SMMU-backed uncompressed memory must refuse backing growth";
	auto *prefix = switch_gpu_backing_register(pages.data(), 8, 0, nullptr, nullptr);
	ASSERT_EQ(prefix, b);
	void *reused = nullptr;
	ASSERT_EQ(switch_gpu_backing_memory_existing(prefix, 8, destroyMemory, &reused), 0);
	EXPECT_EQ(reused, object);
	switch_gpu_backing_release(prefix);
	switch_gpu_backing_release(b);
}
