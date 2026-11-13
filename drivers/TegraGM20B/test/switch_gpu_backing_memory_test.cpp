// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuBackingMemory.h"
#include <functional>

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
