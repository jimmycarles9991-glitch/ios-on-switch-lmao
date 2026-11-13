// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using u32 = uint32_t;

struct refcount_t {
	unsigned value;
};

void refcount_inc(refcount_t *r)
{
	++r->value;
}

void refcount_set(refcount_t *r, unsigned n)
{
	r->value = n;
}

bool refcount_dec_and_test(refcount_t *r)
{
	return --r->value == 0;
}

__attribute__((no_thread_safety_analysis)) void mutex_lock(std::mutex *lock)
{
	lock->lock();
}

__attribute__((no_thread_safety_analysis)) void mutex_unlock(std::mutex *lock)
{
	lock->unlock();
}

struct nvkm_mm_node {
	u32 offset, length;
};

struct nvkm_mm {
	unsigned allocations = 0, releases = 0;
	bool full = false;
};

struct nvkm_tags {
	nvkm_mm_node *mn;
	refcount_t refcount;
};

struct nvkm_memory {
	nvkm_tags *tags = nullptr;
	bool switch_tags_held = false;
};

struct nvkm_fb {
	struct {
		std::mutex mutex;
		nvkm_mm mm;
	} tags;
};

struct nvkm_device {
	nvkm_fb *fb;
};

std::atomic<unsigned> warnings, clears;
#define kmalloc_obj(object) static_cast<decltype(&(object))>(calloc(1, sizeof(object)))
#define kfree(object) free(object)
#define WARN_ON(value) ((value) ? (++warnings, true) : false)

int nvkm_mm_head(nvkm_mm *mm, int, int, u32 nr, u32, int, nvkm_mm_node **out)
{
	if (mm->full)
		return -12;
	*out = new nvkm_mm_node{++mm->allocations * 128u, nr};
	return 0;
}

void nvkm_mm_free(nvkm_mm *mm, nvkm_mm_node **node)
{
	if (*node) {
		++mm->releases;
		delete *node;
		*node = nullptr;
	}
}

void clearTags(nvkm_device *, u32, u32)
{
	++clears;
}

#include "drivers/TegraGM20B/SwitchGpuMemoryTags.h"

namespace
{
class MemoryTags : public testing::Test
{
protected:
	nvkm_fb fb;
	nvkm_device device{&fb};
	nvkm_memory memory;

	void SetUp() override
	{
		warnings = clears = 0;
	}

	void TearDown() override
	{
		if (memory.switch_tags_held)
			nvkm_memory_tags_put(&memory, &device, &memory.tags);
		EXPECT_EQ(memory.tags, nullptr);
		EXPECT_EQ(fb.tags.mm.allocations, fb.tags.mm.releases);
		EXPECT_EQ(warnings, 0u);
	}

	nvkm_tags *get(u32 count = 32)
	{
		nvkm_tags *tags = nullptr;
		EXPECT_EQ(nvkm_memory_tags_get(&memory, &device, count, clearTags, &tags), 0);
		return tags;
	}
};
}

TEST_F(MemoryTags, RepeatedRawMapsKeepOneMemoryReference)
{
	auto *first = get();
	auto *identity = first;
	nvkm_memory_tags_hold(&memory, &device, &first);
	EXPECT_EQ(first, nullptr);
	for (unsigned i = 0; i < 200; ++i) {
		auto *next = get();
		EXPECT_EQ(next, identity);
		nvkm_memory_tags_hold(&memory, &device, &next);
		EXPECT_EQ(next, nullptr);
		EXPECT_EQ(memory.tags->refcount.value, 1u);
	}
	EXPECT_EQ(clears, 1u);
}

TEST_F(MemoryTags, ConcurrentFirstMappingsShareExactlyOneOwnedReference)
{
	constexpr unsigned count = 16;
	std::promise<void> all;
	auto ready = all.get_future().share();
	std::atomic<unsigned> entered = 0;
	std::vector<std::thread> maps;
	for (unsigned i = 0; i < count; ++i)
		maps.emplace_back([&] {
			auto *tags = get();
			if (++entered == count)
				all.set_value();
			ready.wait();
			nvkm_memory_tags_hold(&memory, &device, &tags);
		});
	for (auto &map : maps)
		map.join();
	EXPECT_TRUE(memory.switch_tags_held);
	ASSERT_NE(memory.tags, nullptr);
	EXPECT_EQ(memory.tags->refcount.value, 1u);
	EXPECT_EQ(fb.tags.mm.allocations, 1u);
	EXPECT_EQ(clears, 1u);
}

TEST_F(MemoryTags, ManagedMappingReferenceIsIndependentOfRawOwner)
{
	auto *managed = get();
	auto *raw = get();
	nvkm_memory_tags_hold(&memory, &device, &raw);
	EXPECT_EQ(memory.tags->refcount.value, 2u);
	nvkm_memory_tags_put(&memory, &device, &managed);
	EXPECT_EQ(memory.tags->refcount.value, 1u);
	EXPECT_EQ(fb.tags.mm.releases, 0u);
}

TEST_F(MemoryTags, IncompatibleTagExtentCannotReplaceExistingLines)
{
	auto *raw = get();
	nvkm_memory_tags_hold(&memory, &device, &raw);
	auto *identity = memory.tags;
	nvkm_tags *other = nullptr;
	EXPECT_EQ(nvkm_memory_tags_get(&memory, &device, 64, clearTags, &other), -EINVAL);
	EXPECT_EQ(other, nullptr);
	EXPECT_EQ(memory.tags, identity);
	EXPECT_EQ(memory.tags->refcount.value, 1u);
}

TEST_F(MemoryTags, FailedAllocationStillHasOneStableUncompressedDecision)
{
	fb.tags.mm.full = true;
	auto *raw = get();
	ASSERT_NE(raw, nullptr);
	EXPECT_EQ(raw->mn, nullptr);
	nvkm_memory_tags_hold(&memory, &device, &raw);
	fb.tags.mm.full = false;
	raw = get();
	EXPECT_EQ(raw->mn, nullptr);
	nvkm_memory_tags_hold(&memory, &device, &raw);
	EXPECT_EQ(memory.tags->refcount.value, 1u);
	EXPECT_EQ(clears, 0u);
}

TEST_F(MemoryTags, ManagedOnlyReleaseDropsAllTagsWithoutCreatingRawOwner)
{
	auto *managed = get();
	EXPECT_FALSE(memory.switch_tags_held);
	nvkm_memory_tags_put(&memory, &device, &managed);
	EXPECT_EQ(managed, nullptr);
	EXPECT_EQ(memory.tags, nullptr);
	nvkm_memory_tags_hold(&memory, &device, &managed);
	EXPECT_FALSE(memory.switch_tags_held);
}
