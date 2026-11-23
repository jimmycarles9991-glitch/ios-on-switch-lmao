// SPDX-License-Identifier: MIT
// SwitchGpuConnectionQuota.h: one app took every GPU connection, and a respawned backboardd could not open one.
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/SwitchGpuConnectionQuota.h"

namespace
{
constexpr uint32_t kAll = 64;

TEST(ConnectionQuota, AProcessHoldsAtMostItsShare)
{
	SwitchGpuConnectionQuota quota;
	for (uint32_t i = 0; i < SwitchGpuConnectionQuota::kPerTask; ++i)
		ASSERT_TRUE(quota.take(101, false, kAll));
	EXPECT_FALSE(quota.take(101, false, kAll));
	EXPECT_TRUE(quota.take(102, false, kAll));
	quota.give(101);
	EXPECT_TRUE(quota.take(101, false, kAll)) << "a closed connection goes back to its process";
}

TEST(ConnectionQuota, TheLastEighthIsTheUiCriticalProcesses)
{
	SwitchGpuConnectionQuota quota;
	for (int pid = 201; pid < 208; ++pid)
		for (uint32_t i = 0; i < SwitchGpuConnectionQuota::kPerTask; ++i)
			ASSERT_TRUE(quota.take(pid, false, kAll)) << pid;
	EXPECT_FALSE(quota.take(208, false, kAll)) << "56 of 64 taken by apps";
	for (uint32_t i = 0; i < kAll / 8; ++i)
		EXPECT_TRUE(quota.take(300 + int(i), true, kAll)) << "backboardd respawned";
	EXPECT_FALSE(quota.take(400, true, kAll)) << "every connection is held";
}
}  // namespace
