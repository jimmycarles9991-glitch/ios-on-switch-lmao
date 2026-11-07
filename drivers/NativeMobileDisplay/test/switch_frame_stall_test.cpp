// Host test of drivers/NativeMobileDisplay/SwitchFrameStall.h: which frame ends are a stall worth a snapshot.
#include <gtest/gtest.h>

#include "../SwitchFrameStall.h"

namespace {

constexpr uint64_t kPeriod = 16667;

}  // namespace

TEST(SwitchFrameStall, OnceWhenPresentsEveryFrameStop)
{
    SwitchFrameStall s;
    s.present(1000000, kPeriod, kPeriod);
    const uint64_t last = 1000000 + kPeriod;
    s.present(last, kPeriod, kPeriod);
    EXPECT_FALSE(s.frameEnd(last + kPeriod, kPeriod, true));
    EXPECT_FALSE(s.frameEnd(last + 3 * kPeriod - 1, kPeriod, true));
    EXPECT_TRUE(s.frameEnd(last + 3 * kPeriod, kPeriod, true));
    EXPECT_EQ(s.lastPresent(), last);
    EXPECT_FALSE(s.frameEnd(last + 4 * kPeriod, kPeriod, true));
    s.present(last + 5 * kPeriod, 5 * kPeriod, kPeriod);  // the stall's own present arms nothing
    EXPECT_FALSE(s.frameEnd(last + 9 * kPeriod, kPeriod, true));
    s.present(last + 10 * kPeriod, 5 * kPeriod, kPeriod);
    s.present(last + 11 * kPeriod, kPeriod, kPeriod);  // every frame again
    EXPECT_TRUE(s.frameEnd(last + 14 * kPeriod, kPeriod, true));
}

TEST(SwitchFrameStall, NotForSlowClientsStillScreensOrNoVsyncClient)
{
    SwitchFrameStall s;
    s.present(1000000, 3 * kPeriod, kPeriod);  // a 20 Hz client: its frames are not late
    EXPECT_FALSE(s.frameEnd(1000000 + 4 * kPeriod, kPeriod, true));
    s.present(2000000, kPeriod * 3 / 2, kPeriod);
    EXPECT_FALSE(s.frameEnd(2000000 + 3 * kPeriod, kPeriod, false));
    EXPECT_TRUE(s.frameEnd(2000000 + 3 * kPeriod, kPeriod, true));
    s.present(3000000, 0, kPeriod);  // the first present: no interval
    EXPECT_FALSE(s.frameEnd(3000000 + 10 * kPeriod, kPeriod, true));
    EXPECT_FALSE(SwitchFrameStall().frameEnd(10 * kPeriod, kPeriod, true));
}
