// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdlib>
#include "switch_host1x_results_calls.h"
#include "switch_host1x_results_stubs.h"

namespace {
IOLock locks[2]{};
uint32_t allocatedLocks = 0, allocatedRecords = 0;
}

IOLock *IOLockAlloc()
{
    if (allocatedLocks == 2)
        return nullptr;
    return &locks[allocatedLocks++];
}

void IOLockLock(IOLock *lock)
{
    ASSERT_NE(lock, nullptr) << "Kernel translation unit observed an uninitialized result lock";
    EXPECT_FALSE(lock->held);
    lock->held = true;
}

void IOLockUnlock(IOLock *lock)
{
    ASSERT_NE(lock, nullptr);
    EXPECT_TRUE(lock->held);
    lock->held = false;
}

void IOLockWakeup(IOLock *lock, void *, bool)
{
    ASSERT_NE(lock, nullptr);
    EXPECT_TRUE(lock->held);
}

void IOLockSleep(IOLock *, void *, int)
{
    ADD_FAILURE() << "Completed result unexpectedly tried to sleep";
    std::abort();
}

void *switch_host1x_test_alloc(size_t count, size_t bytes)
{
    ++allocatedRecords;
    return std::calloc(count, bytes);
}

TEST(SwitchHost1xResultsLinkage, KernelTranslationUnitsShareBothEngineTimelines)
{
    ASSERT_TRUE(switch_host1x_test_start(SWITCH_GPU_ENGINE_VIC, 167));
    ASSERT_TRUE(switch_host1x_test_start(SWITCH_GPU_ENGINE_NVDEC, 162));
    EXPECT_EQ(allocatedLocks, 2u);
    for (const uint32_t engine : {uint32_t(SWITCH_GPU_ENGINE_VIC), uint32_t(SWITCH_GPU_ENGINE_NVDEC)}) {
        const uint32_t syncpoint = engine == SWITCH_GPU_ENGINE_VIC ? 167 : 162;
        ASSERT_TRUE(switch_host1x_test_start(engine, syncpoint));
        EXPECT_FALSE(switch_host1x_test_start(engine, syncpoint + 1));
        switch_gpu_backing_token first{}, independent{};
        const uint32_t allocations = allocatedRecords;
        auto *record = switch_host1x_test_produce(engine, 7, 9, &first);
        auto *other = switch_host1x_test_consumer_allocate(engine, 10, &independent);
        ASSERT_NE(record, nullptr);
        ASSERT_NE(other, nullptr);
        ASSERT_NE(record, other);
        EXPECT_EQ(allocatedRecords, allocations);
        EXPECT_EQ(first.engine, engine);
        EXPECT_EQ(first.queue, syncpoint);
        EXPECT_EQ(independent.queue, syncpoint);
        ASSERT_EQ(switch_host1x_test_status(&first), 0);
        ASSERT_EQ(switch_host1x_test_producer_status(&independent), 0);
        ASSERT_EQ(switch_host1x_test_retain(&first), 0);
        switch_host1x_test_physical(engine, record, 0);
        EXPECT_EQ(switch_host1x_test_physical_status(&first), 1);
        EXPECT_EQ(switch_host1x_test_producer_status(&first), 0);
        switch_host1x_test_semantic(&first, -5);
        EXPECT_EQ(switch_host1x_test_producer_status(&first), -5);
        switch_host1x_test_complete(engine, &record, -5);
        switch_host1x_test_complete(engine, &other, 0);
        EXPECT_EQ(record, nullptr);
        EXPECT_EQ(other, nullptr);
        EXPECT_EQ(switch_host1x_test_history(engine, 8), -5);
        EXPECT_EQ(switch_host1x_test_status(&independent), 1);
        for (uint64_t sequence = 11; sequence < 90; ++sequence) {
            switch_gpu_backing_token token{};
            record = switch_host1x_test_produce(engine, sequence, sequence, &token);
            ASSERT_NE(record, nullptr);
            switch_host1x_test_complete(engine, &record, 0);
            ASSERT_EQ(switch_host1x_test_status(&token), 1);
        }
        EXPECT_EQ(switch_host1x_test_status(&first), -5);
        switch_host1x_test_release(&first);
        EXPECT_EQ(switch_host1x_test_producer_status(&first), -5);
        EXPECT_EQ(switch_host1x_test_history(engine, 8), -5);
    }
    EXPECT_EQ(allocatedLocks, 2u);
}
