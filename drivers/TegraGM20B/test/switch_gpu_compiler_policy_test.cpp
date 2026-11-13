// SPDX-License-Identifier: MIT
// SwitchGpuShaderCompilerPolicy.h: when the shader compiler broker stops waiting for a compiler, so that no GPU client
// sleeps on a request no compiler will ever take: a decline stands, a compiler nothing restarts is a decline, and a
// restarted one is waited for until its third loss in a row with no registration between.
#include "SwitchGpuShaderCompilerPolicy.h"

#include <gtest/gtest.h>

namespace
{

using switch_gpu_compiler::declinedAfterClose;
using switch_gpu_compiler::kLosses;
using switch_gpu_compiler::registeredAfterLosses;

TEST(CompilerPolicy, ADeclineStandsWhenTheDeclinerCloses)
{
	uint32_t losses = 0;
	EXPECT_TRUE(declinedAfterClose(true, true, losses));
	EXPECT_EQ(losses, 0u);
}

TEST(CompilerPolicy, ACompilerNothingStartsAgainIsADecline)
{
	uint32_t losses = 0;
	EXPECT_TRUE(declinedAfterClose(false, false, losses));
}

TEST(CompilerPolicy, ARestartedCompilerIsWaitedForUntilItsThirdLossInARow)
{
	uint32_t losses = 0;
	for (uint32_t i = 1; i < kLosses; ++i)
		EXPECT_FALSE(declinedAfterClose(false, true, losses)) << i;
	EXPECT_TRUE(declinedAfterClose(false, true, losses));
	EXPECT_EQ(losses, kLosses);
}

// T-v3g: the 6 MB limit killed a serving compiler 17 times, launchd started each again and each registered, yet from the
// third loss on the kernel ended the requests it held Unavailable and their processes compiled the states themselves
TEST(CompilerPolicy, ACompilerKilledWhileItServesIsWaitedForEveryTime)
{
	uint32_t losses = 0;
	for (uint32_t i = 0; i < 4 * kLosses; ++i) {
		registeredAfterLosses(losses);
		EXPECT_FALSE(declinedAfterClose(false, true, losses)) << i;
	}
}

TEST(CompilerPolicy, ALaunchersLastStartIsADeclineWhenItCloses)
{
	uint32_t losses = 0;
	registeredAfterLosses(losses);
	EXPECT_FALSE(declinedAfterClose(false, true, losses));
	registeredAfterLosses(losses);
	EXPECT_TRUE(declinedAfterClose(false, false, losses));
}

}  // namespace
