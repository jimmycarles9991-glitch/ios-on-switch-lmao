// Host test of drivers/TegraGM20BMetal/plugin/SwitchFrameTimeline.h: which command buffers get a SwitchFrameUser line.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "SwitchFrameTimeline.h"

extern "C" int kdebug_trace(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return 0;
}

namespace {

using switch_frame::Stamps;

uint64_t ms(uint64_t n)
{
    return n * switch_frame::ticksPerMs();
}

// A command buffer committed now, the thread's previous commit `gap` ago, made `recording` ago.
void commitAfter(Stamps &s, switch_frame::Since &since, uint64_t gap, uint64_t recording = 0)
{
    const uint64_t now = mach_absolute_time();
    since.commit = now - gap;
    if (recording)
        s.set(switch_frame::Made, now - recording);
    switch_frame::committed(s, since, {});
}

}  // namespace

// The first command buffer of a frame that came late opens it; the ones the thread commits right after it are the same
// frame's and get a line too, with their own encode, wait, object and import figures.
TEST(SwitchFrameTimeline, EveryCommandBufferOfASlowFrameIsSlow)
{
    switch_frame::Since since;
    Stamps first;
    commitAfter(first, since, ms(30));
    EXPECT_TRUE(switch_frame::slow(first));
    Stamps second;
    switch_frame::committed(second, since, {});
    EXPECT_TRUE(switch_frame::slow(second)) << "committed straight after the slow one";
    Stamps next;
    commitAfter(next, since, ms(16));
    EXPECT_FALSE(switch_frame::slow(next)) << "the next frame, on time";
}

// A gap of 250 ms or more is a still screen unless the thread spent it on the command buffer: recording it for 350 ms
// is a stall worth a line.
TEST(SwitchFrameTimeline, AThreadBusyOver250MsIsReportedAnIdleOneIsNot)
{
    switch_frame::Since since;
    Stamps idle;
    commitAfter(idle, since, ms(400), ms(2));
    EXPECT_FALSE(switch_frame::slow(idle));
    Stamps recording;
    commitAfter(recording, since, ms(400), ms(350));
    EXPECT_TRUE(switch_frame::slow(recording));
}

// Time blocked in waitUntilScheduled since the thread's last commit counts as busy, as recording does.
TEST(SwitchFrameTimeline, AThreadBlockedInWaitUntilScheduledOver250MsIsReported)
{
    switch_frame::Since since;
    switch_frame::blockedTicks() = ms(380);
    Stamps blocked;
    commitAfter(blocked, since, ms(400), ms(2));
    EXPECT_TRUE(switch_frame::slow(blocked));
    EXPECT_EQ(blocked.blockedBefore, ms(380));
    EXPECT_EQ(switch_frame::blockedTicks(), 0u) << "the next commit starts from none";
}

// A command buffer whose waitUntilScheduled blocked over 8 ms until its scheduled handlers ran gets a line of its own,
// however its frame went; the wait's first start is kept and the thread's blocked time grows by each wait.
TEST(SwitchFrameTimeline, ALongWaitUntilScheduledIsSlowOnItsOwn)
{
    switch_frame::Since since;
    Stamps s;
    commitAfter(s, since, ms(16));
    switch_frame::blockedTicks() = 0;
    {
        const switch_frame::WaitScope wait(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint64_t first = s.get(switch_frame::WaitStart);
    ASSERT_NE(first, 0u);
    EXPECT_GE(switch_frame::blockedTicks(), ms(1));
    {
        const switch_frame::WaitScope again(s);
    }
    EXPECT_EQ(s.get(switch_frame::WaitStart), first);
    s.set(switch_frame::Scheduled, first + ms(5));
    EXPECT_EQ(switch_frame::slowReasons(s), 0u);
    s.set(switch_frame::Scheduled, first + ms(149));
    EXPECT_EQ(switch_frame::slowReasons(s), unsigned(switch_frame::kSlowWait));
    char why[32];
    switch_frame::describeSlow(switch_frame::kSlowFrame | switch_frame::kSlowWait, why, sizeof(why));
    EXPECT_STREQ(why, "frame,wait");
    switch_frame::blockedTicks() = 0;
}
