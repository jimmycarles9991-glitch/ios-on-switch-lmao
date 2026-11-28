// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchHost1xMlockProbe.h: the module lock probe's two streams, with and without the lock (opcode 14).
#include <gtest/gtest.h>

#include "../SwitchHost1xMlockProbe.h"

namespace {
using namespace switch_host1x_mlock;

TEST(Host1xMlockProbe, LockOpcodesAreL4TsOpcodeFourteen)
{
    EXPECT_EQ(tegra_host1x_op_acquire_mlock(10), 0xe000000au);
    EXPECT_EQ(tegra_host1x_op_release_mlock(10), 0xe100000au);
}

// The holder takes the lock, says so, waits on the gate, lets the lock go, and only then raises its job's fence.
TEST(Host1xMlockProbe, TheHolderRaisesItsFenceOnlyAfterLettingTheLockGo)
{
    uint32_t w[kHolderWords] = {};
    ASSERT_EQ(holder(w, true, 181, 180, 7, 182), kHolderWords);
    const uint32_t expected[] = {
        0xe000000au,
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
        tegra_host1x_op_incr_syncpt(0, 181),
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5),
        7,
        180,
        0xe100000au,
        tegra_host1x_op_incr_syncpt(0, 182),
    };
    for (uint32_t i = 0; i < kHolderWords; ++i)
        EXPECT_EQ(w[i], expected[i]) << i;
    EXPECT_EQ(holder(w, false, 181, 180, 7, 182), kHolderWords - 2) << "the control holds no lock";
    EXPECT_EQ(w[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0));
}

TEST(Host1xMlockProbe, TheAskerRunsOnlyThroughTheLock)
{
    uint32_t w[kAskerWords] = {};
    ASSERT_EQ(asker(w, true, 183), kAskerWords);
    EXPECT_EQ(w[0], 0xe000000au);
    EXPECT_EQ(w[1], 0xe100000au);
    EXPECT_EQ(w[3], tegra_host1x_op_incr_syncpt(0, 183));
    EXPECT_EQ(asker(w, false, 183), 2u);
    EXPECT_EQ(w[1], tegra_host1x_op_incr_syncpt(0, 183));
}
}  // namespace
