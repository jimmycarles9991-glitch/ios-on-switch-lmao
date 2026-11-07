// kits/SwitchRefusal's user-space build: once per key, one winner under races, rate-limited past a full window.
#include "kits/SwitchRefusal/SwitchRefusal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t kPitch = switch_refusal_key("SwitchRefusalTest", "pitch");
static_assert(switch_refusal_key("", "") == 0xaf63bd4c8601b7dfull, "FNV-1a over one zero byte, at compile time");

TEST(SwitchRefusal, FirstHitOnly)
{
    EXPECT_TRUE(switch_refusal_first(kPitch));
    EXPECT_FALSE(switch_refusal_first(kPitch));
    EXPECT_FALSE(switch_refusal_first(kPitch));
    const uint64_t bgra = switch_refusal_key_with(kPitch, 0x42475241);
    EXPECT_TRUE(switch_refusal_first(bgra));
    EXPECT_FALSE(switch_refusal_first(bgra));
    EXPECT_TRUE(switch_refusal_first(switch_refusal_key("OtherDomain", "pitch")));
    EXPECT_NE(switch_refusal_key("ab", "c"), switch_refusal_key("a", "bc"));
}

TEST(SwitchRefusal, RacingFirstHitsLogOnce)
{
    for (uint64_t round = 0; round < 64; round++) {
        const uint64_t key = switch_refusal_key_with(switch_refusal_key("SwitchRefusalTest", "race"), round);
        std::atomic<int> firsts{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; t++) {
            threads.emplace_back([&] {
                firsts += switch_refusal_first(key) ? 1 : 0;
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        EXPECT_EQ(firsts.load(), 1);
    }
}

TEST(SwitchRefusal, FullWindowRateLimits)
{
    const uint64_t flood = switch_refusal_key("SwitchRefusalTest", "flood");
    int firsts = 0;
    for (uint64_t n = 0; n < 200000; n++) {
        firsts += switch_refusal_first(switch_refusal_key_with(flood, n)) ? 1 : 0;
    }
    EXPECT_LE(firsts, 1024 + 18);
    EXPECT_GT(firsts, 512);
}

TEST(SwitchRefusal, LogsThroughOsLog)
{
    switch_refusal_log("SwitchRefusalTest", "direct", "host test line");
    switch_refusal("SwitchRefusalTest", "format", "format %u refused", 7u);
    switch_refusal_k(switch_refusal_key_with(kPitch, 7), "SwitchRefusalTest", "pitch", "pitch %u", 7u);
}
}  // namespace
