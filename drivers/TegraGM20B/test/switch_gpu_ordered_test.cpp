// SPDX-License-Identifier: MIT
#include "../SwitchGpuOrdered.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

TEST(GpuOrdered, IndependentScopesDoNotClobberVisibility)
{
	SwitchGpuOrdered scopes;
	const auto a = scopes.enter(0x1000), b = scopes.enter(0x2000);
	ASSERT_NE(a, 0u);
	ASSERT_NE(b, 0u);
	EXPECT_TRUE(scopes.active(0x1000));
	EXPECT_TRUE(scopes.active(0x2000));
	scopes.leave(a);
	EXPECT_FALSE(scopes.active(0x1000));
	EXPECT_TRUE(scopes.active(0x2000));
	scopes.leave(b);
	EXPECT_FALSE(scopes.active(0x2000));
}

TEST(GpuOrdered, NestedScopesKeepOuterVisibilityAndReuseSlots)
{
	SwitchGpuOrdered scopes;
	const auto outer = scopes.enter(0x1000), inner = scopes.enter(0x1000);
	ASSERT_NE(outer, inner);
	scopes.leave(inner);
	EXPECT_TRUE(scopes.active(0x1000));
	scopes.leave(outer);
	EXPECT_FALSE(scopes.active(0x1000));
	EXPECT_EQ(scopes.enter(0x1000), outer);
}

TEST(GpuOrdered, SaturationDoesNotSuppressAnotherThreadsFlush)
{
	SwitchGpuOrdered scopes;
	std::array<unsigned, SwitchGpuOrdered::kSlots> ids{};
	for (unsigned i = 0; i < ids.size(); ++i) {
		ids[i] = scopes.enter((i + 1) * 0x1000);
		ASSERT_NE(ids[i], 0u);
	}
	EXPECT_EQ(scopes.enter(0x100000), 0u);
	EXPECT_FALSE(scopes.active(0x100000));
	scopes.leave(0);
	for (unsigned i = 0; i < ids.size(); ++i)
		EXPECT_TRUE(scopes.active((i + 1) * 0x1000));
	scopes.leave(ids[2]);
	ASSERT_NE(scopes.enter(0x100000), 0u);
	EXPECT_TRUE(scopes.active(0x100000));
}

TEST(GpuOrdered, ConcurrentScopesRetainTheirOwnFlushDecision)
{
	SwitchGpuOrdered scopes;
	std::barrier barrier(16);
	std::atomic<unsigned> failures = 0;
	std::vector<std::thread> threads;
	for (unsigned i = 0; i < 16; ++i)
		threads.emplace_back([&, i] {
			const uintptr_t owner = (i + 1) * 0x1000;
			for (unsigned iteration = 0; iteration < 1000; ++iteration) {
				const auto scope = scopes.enter(owner);
				barrier.arrive_and_wait();
				if (!scope || !scopes.active(owner) || scopes.active(0x100000))
					++failures;
				barrier.arrive_and_wait();
				scopes.leave(scope);
				if (scopes.active(owner))
					++failures;
				barrier.arrive_and_wait();
			}
		});
	for (auto &thread : threads)
		thread.join();
	EXPECT_EQ(failures, 0u);
}
