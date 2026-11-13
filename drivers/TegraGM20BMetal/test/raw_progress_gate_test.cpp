// SPDX-License-Identifier: MIT
// raw/runtime/raw_progress_gate.h: a completion wakes the submit worker only while a head is parked, and a head the worker
// parks is never left asleep past the completion it waits for
#include <dispatch/dispatch.h>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "../raw/runtime/raw_progress_gate.h"

namespace {

TEST(ProgressGate, ACompletionWithNoHeadParkedWakesNothing)
{
    raw::ProgressGate gate;
    EXPECT_FALSE(gate.progressed());
    const uint64_t seen = gate.visit();
    EXPECT_FALSE(gate.progressed());
    EXPECT_NE(gate.visit(), seen);
}

// The completion lands after the worker judged the head blocked and before it parks it: the completion sees no head
// parked and wakes nothing, and the worker, about to park, is told to judge the head again
TEST(ProgressGate, ACompletionBetweenJudgingAndParkingSendsTheHeadBackToBeJudged)
{
    raw::ProgressGate gate;
    const uint64_t judged = gate.visit();
    EXPECT_FALSE(gate.progressed());
    EXPECT_FALSE(gate.mayPark(judged));
}

TEST(ProgressGate, ACompletionAfterTheHeadParkedWakesTheWorker)
{
    raw::ProgressGate gate;
    const uint64_t judged = gate.visit();
    ASSERT_TRUE(gate.mayPark(judged));
    EXPECT_TRUE(gate.progressed());
}

// A completion before the worker judged the head is one whose progress the judging saw: the head parks
TEST(ProgressGate, ACompletionBeforeJudgingLetsTheHeadPark)
{
    raw::ProgressGate gate;
    EXPECT_FALSE(gate.progressed());
    EXPECT_TRUE(gate.mayPark(gate.visit()));
}

TEST(ProgressGate, OnlyAWorkerWithNoHeadParkedStopsTheWakes)
{
    raw::ProgressGate gate;
    ASSERT_TRUE(gate.mayPark(gate.visit()));
    gate.idle(true);
    EXPECT_TRUE(gate.progressed());
    gate.idle(false);
    EXPECT_FALSE(gate.progressed());
}

// A worker that parks its one head until a completer's progress, and a completer that makes it at any moment, round after
// round: no round leaves the worker asleep after the progress it waits for (a wait past a second is a lost wake)
TEST(ProgressGate, NoParkedHeadSleepsThroughItsCompletion)
{
    constexpr uint32_t kRounds = 100000;
    raw::ProgressGate gate;
    dispatch_semaphore_t wake = dispatch_semaphore_create(0);
    std::atomic<uint32_t> started{0}, progress{0};
    std::atomic<uint32_t> lost{0}, parks{0};
    std::thread completer([&] {
        for (uint32_t round = 1; round <= kRounds; round++) {
            while (started.load(std::memory_order_acquire) != round)
                std::this_thread::yield();
            for (volatile uint32_t spin = 0; spin < round % 97; spin = spin + 1) {
            }
            progress.store(round, std::memory_order_relaxed);
            if (gate.progressed())
                dispatch_semaphore_signal(wake);
        }
    });
    for (uint32_t round = 1; round <= kRounds; round++) {
        gate.idle(false);
        started.store(round, std::memory_order_release);
        for (;;) {
            const uint64_t judged = gate.visit();
            if (progress.load(std::memory_order_relaxed) >= round)
                break;
            for (volatile uint32_t spin = 0; spin < round % 53; spin = spin + 1) {  // the judging's own time
            }
            if (!gate.mayPark(judged))
                continue;
            parks++;
            if (dispatch_semaphore_wait(wake, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC))) {
                lost++;
                break;
            }
        }
    }
    completer.join();
    EXPECT_EQ(lost.load(), 0u);
    EXPECT_GT(parks.load(), 0u) << "the worker parked in some rounds, so the wakes were exercised";
}

}  // namespace
