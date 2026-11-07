// XudcIsoch.h: the frame list's layout as 21H461 has it, service intervals and stamps, one TRB per frame, and the
// accounting of an isochronous ring's events against a model controller that works through the TDs in order.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "drivers/SwitchUSB/XudcIsoch.h"

namespace {
using namespace xudc;

constexpr uint32_t kSlots = 255, kRequests = 16, kFrames = 64;
using Ring = IsocRing<kSlots, kRequests, kFrames>;

TEST(XudcIsoch, FrameLayoutIsTheFamilys)
{
    EXPECT_EQ(offsetof(IsocFrame, status), 0u);
    EXPECT_EQ(offsetof(IsocFrame, requestCount), 4u);
    EXPECT_EQ(offsetof(IsocFrame, actualCount), 6u);
    EXPECT_EQ(offsetof(IsocFrame, timeStamp), 8u);
    EXPECT_EQ(offsetof(IsocFrame, frameNumber), 16u);
}

TEST(XudcIsoch, ServiceInterval)
{
    EXPECT_EQ(isocIntervalNs(false, 1), 125000u);
    EXPECT_EQ(isocIntervalNs(false, 4), 1000000u);  // a USB audio stream's 1 ms at high speed
    EXPECT_EQ(isocIntervalNs(true, 1), 1000000u);
    EXPECT_EQ(isocIntervalNs(false, 0), 125000u);
    EXPECT_EQ(isocIntervalNs(false, 40), 125000ull << 15);
}

TEST(XudcIsoch, StampsWalkBackFromTheLastFrame)
{
    IsocFrame frames[4] = {};
    frames[2].timeStamp = kIsocNoStamp;
    isocStamp(frames, 4, 10000, 1000);
    EXPECT_EQ(frames[3].timeStamp, 10000u);
    EXPECT_EQ(frames[2].timeStamp, kIsocNoStamp);
    EXPECT_EQ(frames[1].timeStamp, 8000u);
    EXPECT_EQ(frames[0].timeStamp, 7000u);
}

TEST(XudcIsoch, FramesGoDirectOrThroughOnePage)
{
    const xudc::Run runs[] = {{0x80000000, 4096}, {0x90000000, 4096}};
    Piece p;
    ASSERT_TRUE(planFrame(runs, 2, 192, 192, true, 1ull << 32, &p));
    EXPECT_FALSE(p.bounce);
    EXPECT_EQ(p.pa, 0x800000c0u);
    ASSERT_TRUE(planFrame(runs, 2, 4000, 192, false, 1ull << 32, &p));  // across the two runs
    EXPECT_TRUE(p.bounce);
    EXPECT_EQ(p.offset, 4000u);
    ASSERT_TRUE(planFrame(runs, 2, 4096 + 100, 196, false, 1ull << 32, &p));  // IN: no cache-line rule
    EXPECT_FALSE(p.bounce);
    EXPECT_EQ(p.pa, 0x90000064u);
    const xudc::Run small[] = {{0x80000010, 100}};  // OUT inside a run that does not own its lines
    ASSERT_TRUE(planFrame(small, 1, 0, 100, true, 1ull << 32, &p));
    EXPECT_TRUE(p.bounce);
    const xudc::Run high[] = {{0x1ffffff00, 4096}};
    ASSERT_TRUE(planFrame(high, 1, 0, 512, false, 1ull << 32, &p));
    EXPECT_TRUE(p.bounce);
    const xudc::Run boundary[] = {{0x8000ff00, 4096}};
    ASSERT_TRUE(planFrame(boundary, 1, 0, 512, false, 1ull << 32, &p));
    EXPECT_TRUE(p.bounce);
    ASSERT_TRUE(planFrame(runs, 2, 50, 0, true, 1ull << 32, &p));
    EXPECT_FALSE(p.bounce);
    EXPECT_EQ(p.length, 0u);
    EXPECT_FALSE(planFrame(runs, 2, 8100, 192, false, 1ull << 32, &p));  // past the runs
    EXPECT_FALSE(planFrame(runs, 2, 0, 4097, false, 1ull << 32, &p));
}

// The model controller: TDs in ring order; IN raises the request's last frame's event, OUT also a short frame's.
struct Model {
    Ring ring{};
    uint32_t enqueue = 0;

    uint16_t push(const std::vector<uint16_t> &lengths)
    {
        const uint16_t first = (uint16_t)enqueue;
        ring.push(first, lengths.data(), (uint16_t)lengths.size());
        enqueue = (enqueue + (uint32_t)lengths.size()) % kSlots;
        return first;
    }
};

TEST(XudcIsoch, InRequestCompletesOnItsLastFrame)
{
    Model m;
    const uint16_t first = m.push({192, 192, 192, 192});
    EXPECT_EQ(m.ring.event(first + 3u, COMP_SUCCESS, 0), IsocVerdict::Recorded);
    ASSERT_EQ(m.ring.complete(), 1u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(m.ring.request(0).frame[i].state, Ring::kDone);
        EXPECT_EQ(m.ring.request(0).frame[i].actual, 192);
    }
    m.ring.pop();
    EXPECT_EQ(m.ring.onRing(), 0u);
    EXPECT_EQ(m.ring.room(), kSlots - 1);
}

TEST(XudcIsoch, OutShortFramesKeepTheirOwnLengths)
{
    Model m;
    const uint16_t first = m.push({200, 200, 200});
    EXPECT_EQ(m.ring.event(first + 1u, COMP_SHORT_PACKET, 8), IsocVerdict::Recorded);
    EXPECT_EQ(m.ring.complete(), 0u);
    EXPECT_EQ(m.ring.event(first + 2u, COMP_SHORT_PACKET, 200), IsocVerdict::Recorded);
    ASSERT_EQ(m.ring.complete(), 1u);
    const auto &r = m.ring.request(0);
    EXPECT_EQ(r.frame[0].actual, 200);  // no event: it filled
    EXPECT_EQ(r.frame[1].actual, 192);
    EXPECT_EQ(r.frame[2].actual, 0);
    EXPECT_EQ(r.frame[2].state, Ring::kDone);
}

TEST(XudcIsoch, MissedFrameFailsAloneAndTheRequestGoesOn)
{
    Model m;
    const uint16_t first = m.push({100, 100, 100, 100});
    EXPECT_EQ(m.ring.event(first + 1u, COMP_MISSED_SERVICE, 0), IsocVerdict::Recorded);
    EXPECT_EQ(m.ring.event(first + 3u, COMP_SUCCESS, 0), IsocVerdict::Recorded);
    ASSERT_EQ(m.ring.complete(), 1u);
    const auto &r = m.ring.request(0);
    EXPECT_EQ(r.frame[1].state, Ring::kFailed);
    EXPECT_EQ(r.frame[1].actual, 0);
    EXPECT_EQ(r.frame[0].state, Ring::kDone);
    EXPECT_EQ(r.frame[3].state, Ring::kDone);
}

TEST(XudcIsoch, ALaterRequestsEventSettlesTheOnesBefore)
{
    Model m;
    m.push({64, 64});
    m.push({64, 64});
    const uint16_t third = m.push({64, 64});
    EXPECT_EQ(m.ring.event(third + 1u, COMP_SUCCESS, 0), IsocVerdict::Recorded);
    EXPECT_EQ(m.ring.complete(), 3u);
    for (uint32_t k = 0; k < 3; ++k)
        for (uint32_t i = 0; i < 2; ++i)
            EXPECT_EQ(m.ring.request(k).frame[i].actual, 64);
}

TEST(XudcIsoch, ResidueLargerThanTheFrameFailsIt)
{
    Model m;
    const uint16_t first = m.push({64});
    m.ring.event(first, COMP_SHORT_PACKET, 65);
    ASSERT_EQ(m.ring.complete(), 1u);
    EXPECT_EQ(m.ring.request(0).frame[0].state, Ring::kFailed);
}

TEST(XudcIsoch, StarvedAndUnexplainedEventsChangeNothing)
{
    Model m;
    const uint16_t first = m.push({64, 64});
    EXPECT_EQ(m.ring.event(~0u, COMP_RING_UNDERRUN, 0), IsocVerdict::Starved);
    EXPECT_EQ(m.ring.event(first + 50u, COMP_SUCCESS, 0), IsocVerdict::Unexplained);
    EXPECT_EQ(m.ring.event(~0u, COMP_SUCCESS, 0), IsocVerdict::Unexplained);
    EXPECT_EQ(m.ring.complete(), 0u);
    EXPECT_EQ(m.ring.request(0).frame[0].state, Ring::kPending);
}

TEST(XudcIsoch, CancelFencesUntilLifted)
{
    Model m;
    const uint16_t first = m.push({64, 64});
    EXPECT_TRUE(m.ring.cancel());
    EXPECT_EQ(m.ring.room(), 0u);
    EXPECT_EQ(m.ring.event(first + 1u, COMP_SUCCESS, 0), IsocVerdict::Stale);
    m.ring.liftFence();
    EXPECT_EQ(m.ring.room(), kSlots - 1);
    Ring empty{};
    EXPECT_FALSE(empty.cancel());
}

// A long stream around the ring: requests of 1 to 8 frames, IN and OUT, each completed by its own last event or by a
// later request's, frames where the model says.
TEST(XudcIsoch, LongStreamWrapsTheRing)
{
    for (const bool out : {false, true}) {
        Model m;
        uint32_t seed = 7, completed = 0, expectedFrames = 0, seenFrames = 0;
        for (int round = 0; round < 20000; ++round) {
            seed = seed * 1103515245u + 12345u;
            const uint32_t frames = 1 + (seed >> 16) % 8;
            if (m.ring.onRing() < kRequests && m.ring.room() >= frames) {
                std::vector<uint16_t> lengths(frames, 96);
                m.push(lengths);
                expectedFrames += frames;
            }
            if ((seed >> 8) % 3 == 0 && m.ring.onRing() > m.ring.complete()) {
                const auto &r = m.ring.request(m.ring.complete());
                const uint32_t last = (r.first + r.frames - 1u) % kSlots;
                if (out && r.frames > 1 && (seed >> 4) % 2)
                    m.ring.event(r.first, COMP_SHORT_PACKET, 32);
                ASSERT_EQ(m.ring.event(last, COMP_SUCCESS, 0), IsocVerdict::Recorded);
            }
            while (m.ring.complete()) {
                const auto &r = m.ring.request(0);
                for (uint32_t i = 0; i < r.frames; ++i) {
                    ASSERT_EQ(r.frame[i].state, Ring::kDone);
                    ASSERT_TRUE(r.frame[i].actual == 96 || r.frame[i].actual == 64);
                }
                seenFrames += r.frames;
                m.ring.pop();
                ++completed;
            }
        }
        uint32_t left = 0;
        for (uint32_t k = 0; k < m.ring.onRing(); ++k)
            left += m.ring.request(k).frames;
        EXPECT_GT(completed, 1000u);
        EXPECT_EQ(seenFrames + left, expectedFrames);
    }
}
}  // namespace
