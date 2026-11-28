// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.h as a release kernel sees it (DEBUG and DEVELOPMENT undefined): the engines' and the display's channels
// alone, VIC's probe channels and the probes' syncpoints compiled out.
#include <gtest/gtest.h>

#include "../TegraHost1x.h"

#if defined(DEBUG) || defined(DEVELOPMENT)
#error "this test is the release shape"
#endif

TEST(Host1xReleaseShape, AReleaseKernelHasTheEnginesAndTheDisplaysChannelsAlone)
{
    EXPECT_EQ(TEGRA_HOST1X_CHANNELS, TEGRA_HOST1X_CHANNEL_DISPLAY + 1);
}
