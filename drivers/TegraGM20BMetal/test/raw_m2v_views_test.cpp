// SPDX-License-Identifier: MIT
// raw/runtime/raw_m2v_views.h on the host: a range replayed on m2v hands its encoder one viewport and one scissor
// rectangle through the single forms m2v implements (boot 21's iconservicesagent: -[NBRenderEncoder
// setScissorRects:count:] answered zero, so its replays drew unscissored); more than one of either is a draw m2v has no
// form of.
#include <gtest/gtest.h>

#include "../raw/runtime/raw_m2v_views.h"

namespace {

TEST(M2vViews, OneViewportAndOneScissorReplayThroughTheSingleForms)
{
    EXPECT_TRUE(raw::m2vSingleView(0));
    EXPECT_TRUE(raw::m2vSingleView(1));
    EXPECT_FALSE(raw::m2vSingleView(2));
    EXPECT_FALSE(raw::m2vSingleView(16));
    EXPECT_EQ(raw::m2vViewsMissing(1, 1), nullptr);
    EXPECT_EQ(raw::m2vViewsMissing(0, 1), nullptr);
}

TEST(M2vViews, ArraysAreWhatM2vHasNoFormOf)
{
    EXPECT_STREQ(raw::m2vViewsMissing(2, 1), "more than one viewport");
    EXPECT_STREQ(raw::m2vViewsMissing(1, 4), "more than one scissor rectangle");
    EXPECT_STREQ(raw::m2vViewsMissing(3, 3), "more than one viewport");
}

}  // namespace
