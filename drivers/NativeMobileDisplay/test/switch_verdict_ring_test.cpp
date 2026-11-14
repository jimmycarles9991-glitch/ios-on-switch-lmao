// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchVerdictRing.h with publishers on three threads and the consumer on a fourth, as validateSwap's verdicts reach
// the display's gate: every record arrives once and whole, each publisher's in its own order, and one a full ring
// refuses is dropped by its publisher, never half written or overwritten. Built with ThreadSanitizer as well.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../SwitchVerdictRing.h"

namespace {

struct Record {
    uint64_t producer, index, check, pad;
};

using Ring = switch_display::VerdictRing<Record, 32>;

TEST(VerdictRing, EveryRecordOnceAndWholeInEachPublishersOrder)
{
    static Ring ring;
    constexpr unsigned kPublishers = 3, kEach = 20000;
    std::atomic<unsigned> done{0};
    std::atomic<uint64_t> dropped{0};
    std::vector<std::thread> publishers;
    for (unsigned p = 0; p < kPublishers; ++p) {
        publishers.emplace_back([&, p] {
            for (uint64_t i = 1; i <= kEach; ++i) {
                if (!ring.publish({p, i, p * 1000003u ^ i, 0}))
                    ++dropped;
                if (i % 64 == 0)
                    std::this_thread::yield();
            }
            ++done;
        });
    }
    uint64_t received = 0, last[kPublishers] = {};
    bool torn = false, reordered = false;
    const auto take = [&](const Record &r) {
        torn = torn || r.producer >= kPublishers || r.check != (r.producer * 1000003u ^ r.index);
        if (r.producer < kPublishers) {
            reordered = reordered || r.index <= last[r.producer];
            last[r.producer] = r.index;
        }
        ++received;
    };
    while (done < kPublishers)
        ring.drain(take);
    for (auto &t : publishers)
        t.join();
    ring.drain(take);
    EXPECT_FALSE(torn);
    EXPECT_FALSE(reordered);
    EXPECT_EQ(received + dropped, uint64_t(kPublishers) * kEach);
}

TEST(VerdictRing, AFullRingRefusesTheNewestAndKeepsTheRest)
{
    Ring ring;
    for (uint64_t i = 1; i <= 32; ++i)
        EXPECT_TRUE(ring.publish({0, i, i, 0}));
    EXPECT_FALSE(ring.publish({0, 33, 33, 0}));
    std::vector<uint64_t> got;
    ring.drain([&](const Record &r) {
        got.push_back(r.index);
    });
    ASSERT_EQ(got.size(), 32u);
    EXPECT_EQ(got.front(), 1u);
    EXPECT_EQ(got.back(), 32u);
    EXPECT_TRUE(ring.publish({0, 34, 34, 0}));
    got.clear();
    ring.drain([&](const Record &r) {
        got.push_back(r.index);
    });
    EXPECT_EQ(got, std::vector<uint64_t>{34});
}

// An accepted overlay swap's verdict carries its layers to the gate: a swapping thread publishes, the gate thread drains
// and runs its admission between its own kicks and frame ends, and only the gate ever touches what the admission owns.
TEST(VerdictRing, AnOverlaySwapsLayersReachTheGateWhole)
{
    struct Overlay {
        uint64_t index, admit;
        uint64_t layers[64];
    };

    static switch_display::VerdictRing<Overlay, 32> ring;
    constexpr uint64_t kSwaps = 20000;
    std::atomic<bool> done{false};
    uint64_t dropped = 0;
    std::thread swapping([&] {
        for (uint64_t i = 1; i <= kSwaps; ++i) {
            Overlay o = {i, i % 3 == 0, {}};
            for (uint64_t w = 0; w < 64; ++w)
                o.layers[w] = i * 64 + w;
            if (!ring.publish(o))
                ++dropped;
        }
        done = true;
    });
    // The gate's own: what the admission reserved and what its kicks and frame ends read of it.
    uint64_t reserved = 0, kicks = 0, frameEnds = 0, received = 0, last = 0;
    bool torn = false;
    const auto take = [&](const Overlay &o) {
        for (uint64_t w = 0; w < 64; ++w)
            torn = torn || o.layers[w] != o.index * 64 + w;
        if (o.admit)
            reserved += o.layers[0];
        torn = torn || o.index <= last;
        last = o.index;
        ++received;
    };
    while (!done) {
        ring.drain(take);
        kicks += reserved != 0u;
        ++frameEnds;
    }
    swapping.join();
    ring.drain(take);
    EXPECT_FALSE(torn);
    EXPECT_EQ(received + dropped, kSwaps);
    EXPECT_GT(frameEnds, 0u);
    EXPECT_GE(frameEnds, kicks);
}

}  // namespace
