// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../event_job.h"

namespace {
using gm20b::EventRangeView;

EventRangeView encoder()
{
    return {};
}

EventRangeView signal(bool leasable = true, bool carried = false)
{
    return {true, true, leasable, carried};
}

EventRangeView wait()
{
    return {true, false, true, false};
}

TEST(EventJob, ASignalAfterAnM2vRangeCarriesTheKernelSignalsRightAfterIt)
{
    const std::vector<EventRangeView> ranges = {encoder(), signal(), signal(), signal(false), signal()};
    EXPECT_EQ(gm20b::signalJobRanges(ranges, 1), (std::vector<size_t>{1, 2}));
}

TEST(EventJob, ACommandBufferOfSignalsAloneIsOneJob)
{
    EXPECT_EQ(gm20b::signalJobRanges({signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_EQ(gm20b::signalJobRanges({signal(), signal(), signal()}, 0), (std::vector<size_t>{0, 1, 2}));
}

TEST(EventJob, ItStopsAtAnEncoderAWaitOrAnEventTheKernelDoesNotHold)
{
    EXPECT_EQ(gm20b::signalJobRanges({signal(), encoder(), signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_EQ(gm20b::signalJobRanges({signal(), wait(), signal()}, 0), (std::vector<size_t>{0}));
    EXPECT_TRUE(gm20b::signalJobRanges({signal(false), signal()}, 0).empty());
}

TEST(EventJob, SignalsAJobAlreadyCarriesAreNotSignalledTwice)
{
    EXPECT_EQ(gm20b::signalJobRanges({signal(), signal(true, true), signal()}, 0), (std::vector<size_t>{0, 2}));
}

TEST(EventJob, WaitsAndSignalsFillTheFewestJobsEveryWaitFirst)
{
    const auto one = gm20b::eventJobSlices(3, 2, 16);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].waits, 3u);
    EXPECT_EQ(one[0].signals, 2u);
    const auto many = gm20b::eventJobSlices(20, 20, 16);
    ASSERT_EQ(many.size(), 3u);
    EXPECT_EQ(many[0].waits, 16u);
    EXPECT_EQ(many[0].signals, 0u);
    EXPECT_EQ(many[1].firstWait, 16u);
    EXPECT_EQ(many[1].waits, 4u);
    EXPECT_EQ(many[1].signals, 12u);
    EXPECT_EQ(many[2].firstSignal, 12u);
    EXPECT_EQ(many[2].signals, 8u);
    size_t waits = 0, signals = 0;
    bool signalled = false;
    for (const gm20b::EventJobSlice &slice : many) {
        EXPECT_LE(slice.waits + slice.signals, 16u);
        EXPECT_FALSE(signalled && slice.waits) << "a wait after a signal";
        signalled |= slice.signals != 0;
        waits += slice.waits;
        signals += slice.signals;
    }
    EXPECT_EQ(waits, 20u);
    EXPECT_EQ(signals, 20u);
}

TEST(EventJob, NoEventsNoJob)
{
    EXPECT_TRUE(gm20b::eventJobSlices(0, 0, 16).empty());
}

TEST(EventJob, OnlyAJobOnAChannelMadeAgainIsOrderedAgain)
{
    EXPECT_EQ(gm20b::eventJobAnswer(kIOReturnNotResponding), gm20b::EventJobAnswer::reordered);
    EXPECT_EQ(gm20b::eventJobAnswer(kIOReturnNotReady), gm20b::EventJobAnswer::failed);
    EXPECT_EQ(gm20b::eventJobAnswer(kIOReturnBadArgument), gm20b::EventJobAnswer::failed);
}

}  // namespace
