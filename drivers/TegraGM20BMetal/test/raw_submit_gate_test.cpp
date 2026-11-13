// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_submit_gate.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

namespace {

// The submit worker submitting back to back while three threads move: no two of them are ever inside at once, and the
// plain counter they all write (a race TSAN reports were they not) ends at every entry's count
TEST(SubmitGate, SubmissionsAndMovesNeverOverlap)
{
    raw::SubmitGate gate;
    std::atomic<int> inside{0}, overlaps{0};
    uint64_t entries = 0;
    auto enter = [&] {
        overlaps += inside.fetch_add(1) != 0;
        ++entries;
        inside.fetch_sub(1);
    };
    constexpr int kSubmissions = 200000, kMovers = 3, kMoves = 20000;
    std::thread worker([&] {
        for (int i = 0; i < kSubmissions; ++i)
            gate.submit([&] {
                enter();
                return true;
            });
    });
    std::vector<std::thread> movers;
    for (int m = 0; m < kMovers; ++m)
        movers.emplace_back([&] {
            for (int i = 0; i < kMoves; ++i)
                gate.exclude([&] {
                    enter();
                    return true;
                });
        });
    worker.join();
    for (std::thread &mover : movers)
        mover.join();
    EXPECT_EQ(overlaps.load(), 0);
    EXPECT_EQ(entries, uint64_t(kSubmissions) + uint64_t(kMovers) * kMoves);
}

// A move inside a move on one thread (its lock is recursive) and a move with no submission running wait for nothing
TEST(SubmitGate, AMoveNestsAndRunsAtOnceWhileNothingSubmits)
{
    raw::SubmitGate gate;
    int ran = 0;
    gate.exclude([&] {
        ++ran;
        return gate.exclude([&] {
            return ++ran;
        });
    });
    EXPECT_EQ(ran, 2);
    EXPECT_EQ(gate.submit([] {
        return 7;
    }),
              7);
}

}  // namespace
