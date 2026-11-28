// SPDX-License-Identifier: BSD-2-Clause-Patent
// A debug kernel's unlocked wait count on NVJPG's periodic decode line (tegra_nvjpg_lock_counts).
#include "switch_nvjpg_engine_model.h"
#include <atomic>
#include <thread>
#include "../TegraNVJPGStats.h"

namespace {
uint64_t taken()
{
    uint64_t waitsUnlocked = 0;
    tegra_nvjpg_lock_counts(&waitsUnlocked);
    return waitsUnlocked;
}

class NVJPGLockCounts : public NVJPGGate {
protected:
    void SetUp() override
    {
        NVJPGGate::SetUp();
        TegraNVJPGJob job = decodeJob();
        nvjpg_dec_status status = {};
        ASSERT_EQ(runDecode(&job, &status), 0);  // the gate powered on, before any count is read
        taken();
    }

    void TearDown() override
    {
        switch_test_lock_taken = nullptr;
    }
};
}  // namespace

TEST_F(NVJPGLockCounts, EachJobsEngineWaitIsCountedAndTakingTheCountsZeroesThem)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(runDecode(&job, &status), 0);
    ASSERT_EQ(runDecode(&job, &status), 0);
    EXPECT_EQ(taken(), 2u);
    EXPECT_EQ(taken(), 0u);
}

// A client's refused decode takes the counts on its own thread while the engine thread runs jobs.
TEST_F(NVJPGLockCounts, CountsTakenOnAnotherThreadWhileJobsRunLoseNone)
{
    constexpr uint64_t kJobs = 200;
    std::atomic<bool> running{true};
    uint64_t seen = 0;
    std::thread client([&] {
        while (running.load(std::memory_order_acquire))
            seen += taken();
    });
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    for (uint64_t i = 0; i < kJobs; i++)
        ASSERT_EQ(runDecode(&job, &status), 0);
    running.store(false, std::memory_order_release);
    client.join();
    EXPECT_EQ(seen + taken(), kJobs);
}
