// Window B's ordering against the GPU (NativePlaneFence.h), on the host against a model of the GPU present
// (TegraScanoutCopy.h, gpu_present.c present_copy): a present completes once the work its surfaces' writers had submitted is done,
// and its completion interrupt is when the display writes the registers. Over random swaps (a surface the GPU renders or
// not, a window A frame or not, that frame on a GPU present or the CPU copy), no window B write of a surface the GPU renders
// comes before its writer's work is done, none comes later than the present that orders it, and a surface nothing on the
// GPU writes (a video frame) is never held back by a present.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "../NativePlaneFence.h"

using native_planes::planeRoute;
using native_planes::PlaneRoute;

namespace {

uint64_t rng = 0x853c49e6748fea9bull;

uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? static_cast<uint32_t>(rng % n) : 0;
}

// The GPU present: done once every surface it waits for has had its writers' work done, and not before the present's own
// copy or fence has run after its kick.
uint64_t presentDone(uint64_t kick, uint64_t ownWork, uint64_t writersDone)
{
    return std::max(kick + ownWork, writersDone);
}

struct Swap {
    bool fenced;         // window B's new surface is one the GPU renders
    bool uiNew;          // the swap has a window A frame
    bool framePresent;   // that frame goes through a GPU present
    uint64_t kick;       // when the display takes the swap
    uint64_t planeDone;  // when the work the plane surface's writers had submitted is done (fenced only)
    uint64_t uiDone;     // the same for the window A surface's writers
};

Swap randomSwap(uint64_t now)
{
    Swap s = {};
    s.fenced = rnd(2) != 0;
    s.uiNew = rnd(3) != 0;
    s.framePresent = rnd(4) != 0;
    s.kick = now;
    s.planeDone = s.fenced ? now + rnd(20000) : 0;
    s.uiDone = now + rnd(20000);
    return s;
}

}  // namespace

TEST(DisplayPlaneFence, RoutesFollowTheRule)
{
    EXPECT_EQ(planeRoute(false, false, false), PlaneRoute::kNow);
    EXPECT_EQ(planeRoute(true, false, false), PlaneRoute::kFence);
    EXPECT_EQ(planeRoute(true, false, true), PlaneRoute::kFence);
    EXPECT_EQ(planeRoute(false, true, false), PlaneRoute::kWithFrame);
    EXPECT_EQ(planeRoute(false, true, true), PlaneRoute::kWithFrame);
    EXPECT_EQ(planeRoute(true, true, true), PlaneRoute::kWithFrame);
    EXPECT_EQ(planeRoute(true, true, false), PlaneRoute::kNotShown);
}

TEST(DisplayPlaneFence, WindowBNeverOutrunsTheGpu)
{
    unsigned fenced = 0;
    unsigned dropped = 0;
    uint64_t now = 0;
    for (int n = 0; n < 200000; ++n) {
        now += 1 + rnd(17000);
        const Swap s = randomSwap(now);
        const uint64_t ownWork = 50 + rnd(400);
        // When the display writes window B's registers for the swap (UINT64_MAX: never), and the earliest it could.
        uint64_t written = UINT64_MAX;
        uint64_t earliest = s.kick;
        switch (planeRoute(s.fenced, s.uiNew, s.framePresent)) {
        case PlaneRoute::kNow:
            written = s.kick;
            break;
        case PlaneRoute::kWithFrame:
            // The window A frame's present also waits for the plane surface's writers (TegraScanoutCopy alsoWritten) when
            // the GPU renders it; a CPU-copied frame goes out once its copy is done.
            if (s.framePresent) {
                written = presentDone(s.kick, ownWork, std::max(s.uiDone, s.fenced ? s.planeDone : 0));
                earliest = presentDone(s.kick, ownWork, s.uiDone);
            } else {
                written = s.kick + ownWork;
                earliest = written;
            }
            break;
        case PlaneRoute::kFence:
            written = presentDone(s.kick, ownWork, s.planeDone);
            break;
        case PlaneRoute::kNotShown:
            ++dropped;
            break;
        }
        if (written == UINT64_MAX) {
            ASSERT_TRUE(s.fenced && s.uiNew && !s.framePresent) << "only an unfenced frame's GPU change is dropped";
            continue;
        }
        if (s.fenced) {
            ++fenced;
            ASSERT_GE(written, s.planeDone) << "window B written before the GPU finished its surface";
            const uint64_t ordered = s.uiNew ? presentDone(s.kick, ownWork, std::max(s.uiDone, s.planeDone))
                                             : presentDone(s.kick, ownWork, s.planeDone);
            ASSERT_EQ(written, ordered) << "later than the present that orders it";
        } else {
            ASSERT_EQ(written, earliest) << "a surface the GPU does not write waited for a present";
        }
    }
    std::printf("plane fence: %u GPU-rendered sets ordered, %u not shown (frame on the CPU copy)\n", fenced, dropped);
    EXPECT_GT(fenced, 50000u);
}
