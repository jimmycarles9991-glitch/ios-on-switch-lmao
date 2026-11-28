// SPDX-License-Identifier: BSD-2-Clause-Patent
// A debug kernel's timing run against the register model: SE1, proven at bring-up, never ends its first 16 KiB
// operation. The run writes only the driver's own buffers, so SE1 is turned off with one line instead of a panic, and
// the run goes on with SE2 in buffers SE1 was never given. Runs in a process of its own and is given 5 s.
#include "tegra_se_model.h"
#if !(DEVELOPMENT || DEBUG)
#error "the timing run is built only in development and debug kernels"
#endif
#include "bazel/testing/fresh_process.h"

namespace {
void timingRunWithSe1LostOnItsFirstLargeOperation()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    })) << "bring-up did not finish:\n"
        << se_model::log();
    for (AlgState &alg : gAlg)
        alg.on = true;  // the model's engines give no real results, so no known answer passed
    se_model::engines[0].hangFromBytes = 16384;
    ASSERT_TRUE(se_model::finishes([] {
        timing();
    })) << "the timing run did not finish:\n"
        << se_model::log();
    ASSERT_NE(se_model::engines[0].hung, 0u) << "SE1 was never given a 16 KiB operation";
    EXPECT_FALSE(gEngines[0].up);
    EXPECT_TRUE(gEngines[1].up);
    EXPECT_EQ(se_model::count("SE1 off for this boot: operation never ended"), 1u) << se_model::log();
    EXPECT_EQ(se_model::engines[0].started, se_model::engines[0].hung) << "an operation reached SE1 after it was off";
    EXPECT_FALSE(se_model::given.empty());
    EXPECT_EQ(se_model::reused, 0u) << "memory SE1 may still write was given to SE2 or freed";
    EXPECT_GT(se_model::engines[1].started, se_model::engines[0].startedAtHang[1] + 100)
        << "the run did not go on with SE2";
    EXPECT_EQ(se_model::count("TegraSE: cutover"), 1u) << se_model::log();
    EXPECT_TRUE(gAlg[SHA1].on) << "SE1 going off is not SHA-1's fault:\n" << se_model::log();
    EXPECT_EQ(se_model::count("back on the CPU"), 0u) << se_model::log();
}
}  // namespace

TEST(TegraSETiming, AProvenEngineThatNeverEndsATimingOperationIsOffAndTheRunGoesOnWithoutIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(timingRunWithSe1LostOnItsFirstLargeOperation);
}
