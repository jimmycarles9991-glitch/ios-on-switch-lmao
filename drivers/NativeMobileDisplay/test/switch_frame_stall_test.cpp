// Host test of drivers/NativeMobileDisplay/SwitchFrameStall.h: which frame ends are a stall worth a snapshot.
#include <gtest/gtest.h>

#include "../SwitchFrameStall.h"

namespace {

constexpr uint64_t kPeriod = 16667;
constexpr uint64_t kSwapAfter = 9000;  // the render thread's swap, after the frame end it woke at

}  // namespace

TEST(SwitchFrameStall, OnceWhenPresentsEveryFrameStopWithASwapOut)
{
    SwitchFrameStall s;
    s.present(1000000, kPeriod, kPeriod);
    const uint64_t last = 1000000 + kPeriod;
    s.present(last, kPeriod, kPeriod);
    uint64_t accepted = last + kSwapAfter;
    EXPECT_FALSE(s.frameEnd(last + kPeriod, kPeriod, accepted));
    EXPECT_FALSE(s.frameEnd(last + 3 * kPeriod - 1, kPeriod, accepted));
    EXPECT_TRUE(s.frameEnd(last + 3 * kPeriod, kPeriod, accepted));
    EXPECT_EQ(s.lastPresent(), last);
    EXPECT_FALSE(s.frameEnd(last + 4 * kPeriod, kPeriod, accepted));
    s.present(last + 5 * kPeriod, 5 * kPeriod, kPeriod);  // the stall's own present arms nothing
    accepted = last + 5 * kPeriod + kSwapAfter;
    EXPECT_FALSE(s.frameEnd(last + 9 * kPeriod, kPeriod, accepted));
    s.present(last + 10 * kPeriod, 5 * kPeriod, kPeriod);
    s.present(last + 11 * kPeriod, kPeriod, kPeriod);  // every frame again
    accepted = last + 11 * kPeriod + kSwapAfter;
    EXPECT_TRUE(s.frameEnd(last + 14 * kPeriod, kPeriod, accepted));
}

// With no vsync client (Core Animation on iOS 17 and 27), a swap accepted since the last present is what says a frame
// was wanted: without one the presents stopped because the animation ended.
TEST(SwitchFrameStall, NotForSlowClientsEndedAnimationsOrTheFirstPresent)
{
    SwitchFrameStall s;
    s.present(1000000, 3 * kPeriod, kPeriod);  // a 20 Hz client: its frames are not late
    EXPECT_FALSE(s.frameEnd(1000000 + 4 * kPeriod, kPeriod, 1000000 + kSwapAfter));
    s.present(2000000, kPeriod * 3 / 2, kPeriod);
    EXPECT_FALSE(s.frameEnd(2000000 + 3 * kPeriod, kPeriod, 1000000 + kSwapAfter)) << "no swap since that present";
    EXPECT_TRUE(s.frameEnd(2000000 + 3 * kPeriod, kPeriod, 2000000 + kSwapAfter));
    s.present(3000000, 0, kPeriod);  // the first present: no interval
    EXPECT_FALSE(s.frameEnd(3000000 + 10 * kPeriod, kPeriod, 3000000 + kSwapAfter));
    EXPECT_FALSE(SwitchFrameStall().frameEnd(10 * kPeriod, kPeriod, 9 * kPeriod));
}
