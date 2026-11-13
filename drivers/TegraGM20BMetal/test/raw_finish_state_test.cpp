// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_finish_state.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

TEST(FinishState, ReadyWithNothingInFlightFinishesOnceThere)
{
    raw::FinishState s;
    EXPECT_TRUE(s.ready());
    EXPECT_FALSE(s.ready()) << "a second ready() finishes nothing";
}

TEST(FinishState, TheLastCompletionAfterReadyFinishes)
{
    raw::FinishState s;
    s.submitted();
    s.submitted();
    EXPECT_EQ(s.pending(), 2u);
    EXPECT_FALSE(s.completed()) << "not ready yet";
    EXPECT_FALSE(s.ready()) << "one still in flight";
    EXPECT_TRUE(s.completed());
    EXPECT_EQ(s.pending(), 0u);
}

// The submit worker submits ranges and then says ready while the completion thread completes them: in every interleaving
// exactly one of them finishes the command buffer
TEST(FinishState, ExactlyOneThreadFinishesUnderRaces)
{
    for (int round = 0; round < 2000; round++) {
        raw::FinishState s;
        std::atomic<int> finished{0}, handed{0};
        constexpr int kRanges = 4;
        std::thread completion([&] {
            for (int done = 0; done < kRanges;) {
                if (handed.load() > done) {
                    finished += s.completed();
                    done++;
                }
            }
        });
        for (int r = 0; r < kRanges; r++) {
            s.submitted();
            handed++;
        }
        finished += s.ready();
        completion.join();
        EXPECT_EQ(finished.load(), 1) << round;
    }
}

}  // namespace
