// SPDX-License-Identifier: MIT
// scrub.h scrubFate: a scrub's retire reads the fence, then the channel, then the kill record. A reopen (Device::reopen,
// device_channel.cpp) retires the channel's generation and then releases the stand-in fence, which done() reads as
// passed for every seqno; each case here lets a reopen land right after the first read, as the submit worker's can.
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/scrub.h"

namespace {

using gm20b::ScrubFate;

struct Channel {
    bool standIn = false;  // the stand-in fence: done() passes every seqno
    bool live = true;      // the scrub's generation is the device's and its channel not lost
    bool passed = false;   // the real fence past the scrub's seqno
    bool killedRecord = false;
    int reads = 0;
    bool reopenAfterFirst = false;

    void reopen()
    {
        live = false;
        standIn = true;
    }

    template <class T> T read(T value)
    {
        if (++reads == 1 && reopenAfterFirst)
            reopen();
        return value;
    }

    ScrubFate fate()
    {
        return gm20b::scrubFate(
            [&] {
                return read(standIn || passed);
            },
            [&] {
                return read(live);
            },
            [&] {
                return killedRecord;
            });
    }
};

// review-memory2 defect 1: with the channel read first, it reads live, the reopen lands, and done() reads the stand-in
// fence as passed, so the span went back with its fill never run
TEST(ScrubFate, AReopenBetweenTheReadsFillsTheSpanAgain)
{
    Channel c;
    c.reopenAfterFirst = true;
    EXPECT_EQ(c.fate(), ScrubFate::Again);
}

TEST(ScrubFate, AFinishedScrubOnTheLiveChannelIsZeroed)
{
    Channel c;
    c.passed = true;
    EXPECT_EQ(c.fate(), ScrubFate::Zeroed);
}

TEST(ScrubFate, AnUnfinishedScrubOnTheLiveChannelWaits)
{
    Channel c;
    EXPECT_EQ(c.fate(), ScrubFate::Waiting);
}

TEST(ScrubFate, AKilledOrLostScrubIsFilledAgain)
{
    Channel killed;
    killed.passed = true;
    killed.killedRecord = true;
    EXPECT_EQ(killed.fate(), ScrubFate::Again);
    Channel lost;
    lost.reopen();
    EXPECT_EQ(lost.fate(), ScrubFate::Again);
}

}  // namespace
