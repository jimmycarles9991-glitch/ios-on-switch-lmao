// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCompletionFlip.h's word with its writers on their own threads, as the display has them: a producer's filter
// (claimEarly for the arm it was set up for, possibly a stale one), the completion (claim with its status) and the gate
// (arm, withdraw, adopt, present). Each arm is written at most once, never after the gate withdrew it, never by a
// stale claimer, and the gate never sees a write it did not account for. Built with ThreadSanitizer as well.
#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "../SwitchCompletionFlip.h"

namespace {

constexpr int kArms = 4000;

struct Board {
    SwitchCompletionFlip flip;
    std::atomic<unsigned> writes[kArms + 2] = {};
    std::atomic<int> withdrawnAt{0};
    std::atomic<bool> lateWrite{false};

    // A claimer's register stores: counted against the arm it claimed.
    void write(uint32_t generation)
    {
        if (static_cast<int>(generation) == withdrawnAt.load())
            lateWrite = true;
        ++writes[generation];
    }
};

TEST(CompletionFlipClaim, OneWriterPerArmWhateverTheOrder)
{
    Board b;
    std::atomic<uint32_t> armed{0};
    std::atomic<bool> stop{false};
    // The producer's filter: the arm it read, or a stale one.
    std::thread filter([&] {
        std::mt19937 random(3);
        while (!stop) {
            const uint32_t now = armed.load();
            const uint32_t target = random() % 4 == 0 && now > 1 ? now - 1 : now;
            if (b.flip.claimEarly(target, SwitchCompletionFlip::kFromGpuFilter)) {
                b.write(b.flip.generation());
                b.flip.claimed(SwitchCompletionFlip::kEarly);
            }
        }
    });
    // The completion.
    std::thread completion([&] {
        std::mt19937 random(5);
        while (!stop) {
            if (b.flip.claim(random() % 8 == 0 ? -5 : 0)) {
                b.write(b.flip.generation());
                b.flip.claimed(SwitchCompletionFlip::kWritten);
            }
        }
    });
    std::mt19937 random(9);
    for (int arm = 1; arm <= kArms; ++arm) {
        ASSERT_EQ(b.flip.state(), SwitchCompletionFlip::kIdle);
        b.flip.arm(arm % 3, 0x1000u * static_cast<uint32_t>(arm), 0, 2944, 0);
        armed = b.flip.generation();
        for (int spin = static_cast<int>(random() % 64); spin > 0; --spin)
            std::this_thread::yield();
        switch (random() % 3) {
        case 0:
            if (b.flip.withdraw())
                b.withdrawnAt = static_cast<int>(armed.load());
            break;
        case 1:
            (void)b.flip.adopt();
            break;
        default:
            break;
        }
        bool written = false;
        const uint8_t was = b.flip.present(arm % 3, written);
        ASSERT_NE(was, SwitchCompletionFlip::kClaimed);
        ASSERT_EQ(b.flip.state(), SwitchCompletionFlip::kIdle);
        const unsigned n = b.writes[armed.load()].load();
        ASSERT_LE(n, 1u) << "arm " << arm;
        ASSERT_EQ(written, n == 1u) << "arm " << arm << " was " << unsigned(was);
    }
    stop = true;
    filter.join();
    completion.join();
    EXPECT_FALSE(b.lateWrite.load()) << "an arm written after the gate withdrew it";
    unsigned total = 0;
    for (const auto &w : b.writes)
        total += w.load();
    EXPECT_GT(total, 0u);
}

// The generation and the state are one word: a filter left from arm g fails against arm g + 1 whatever the state.
TEST(CompletionFlipClaim, AStaleClaimerFailsAgainstTheNextArm)
{
    SwitchCompletionFlip flip;
    flip.arm(0, 0x1000, 0, 2944, 0);
    const uint32_t first = flip.generation();
    EXPECT_TRUE(flip.withdraw());
    flip.arm(1, 0x2000, 0, 2944, 0);
    EXPECT_FALSE(flip.claimEarly(first, SwitchCompletionFlip::kFromVicFilter));
    EXPECT_EQ(flip.state(), SwitchCompletionFlip::kArmed);
    EXPECT_TRUE(flip.claimEarly(flip.generation(), SwitchCompletionFlip::kFromVicFilter));
    flip.claimed(SwitchCompletionFlip::kEarly);
    EXPECT_FALSE(flip.claim(0)) << "the filter wrote it";
    EXPECT_EQ(flip.state(), SwitchCompletionFlip::kWritten);
}

}  // namespace
