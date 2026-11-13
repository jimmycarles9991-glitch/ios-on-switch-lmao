// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>
#include "../SwitchGpuBoIndex.h"

namespace
{
struct Bo {
	uint64_t handle;
};

struct Memory {
	static inline unsigned allocations, refusals;

	template <typename T> static T *allocate(uint32_t count)
	{
		if (refusals) {
			--refusals;
			return nullptr;
		}
		++allocations;
		return static_cast<T *>(malloc(sizeof(T) * count));
	}

	template <typename T> static void release(T *items, uint32_t)
	{
		free(items);
	}
};

using Index = SwitchGpuBoIndex<Bo, Memory>;
}  // namespace

TEST(BoIndex, HandlesInIssueOrderAreFoundAndFreedOnesAreGone)
{
	Index index;
	std::vector<Bo> bos(1000);
	for (uint64_t i = 0; i < bos.size(); ++i) {
		bos[i].handle = i + 1;
		ASSERT_TRUE(index.reserve());
		index.insert(bos[i].handle, &bos[i]);
	}
	for (const Bo &bo : bos)
		EXPECT_EQ(index.find(bo.handle), &bo);
	EXPECT_EQ(index.find(0), nullptr);
	EXPECT_EQ(index.find(1001), nullptr);
	for (uint64_t handle = 1; handle <= 1000; handle += 3)
		index.erase(handle);
	for (const Bo &bo : bos)
		EXPECT_EQ(index.find(bo.handle), bo.handle % 3 == 1 ? nullptr : &bo);
	index.erase(1);
	EXPECT_EQ(index.size(), 666u);
	index.destroy();
}

TEST(BoIndex, ARandomChurnAgreesWithAMap)
{
	Index index;
	std::map<uint64_t, Bo> model;
	std::mt19937 random(7);
	uint64_t next = 1;
	for (int step = 0; step < 20000; ++step) {
		if (model.empty() || random() % 3) {
			ASSERT_TRUE(index.reserve());
			auto &bo = model[next];
			bo.handle = next++;
			index.insert(bo.handle, &bo);
		} else {
			auto it = model.begin();
			std::advance(it, random() % model.size());
			index.erase(it->first);
			model.erase(it);
		}
		const uint64_t probe = 1 + random() % next;
		const auto it = model.find(probe);
		ASSERT_EQ(index.find(probe), it == model.end() ? nullptr : &it->second);
	}
	EXPECT_EQ(index.size(), model.size());
	index.destroy();
}

TEST(BoIndex, ANewBoIsRefusedBeforeItIsMadeWhenTheIndexCannotGrow)
{
	Index index;
	std::vector<Bo> bos(65);
	for (uint64_t i = 0; i < 64; ++i) {
		bos[i].handle = i + 1;
		ASSERT_TRUE(index.reserve());
		index.insert(bos[i].handle, &bos[i]);
	}
	Memory::refusals = 1;
	EXPECT_FALSE(index.reserve());
	for (uint64_t i = 0; i < 64; ++i)
		EXPECT_EQ(index.find(i + 1), &bos[i]);
	ASSERT_TRUE(index.reserve());
	bos[64].handle = 65;
	index.insert(65, &bos[64]);
	EXPECT_EQ(index.find(65), &bos[64]);
	index.clear();
	EXPECT_EQ(index.find(65), nullptr);
	index.destroy();
}
