// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdint>
using IOByteCount = uint64_t;
constexpr uint32_t kIOMemoryMapperNone = 0x800;
#include "../SwitchAdma.h"

static_assert(!SwitchAdmaHigh::kLiveProofs);

TEST(SwitchReleaseDma, HighMemoryNeverStartsAReadbackSeries)
{
    SwitchAdmaHigh high;
    for (unsigned i = 0; i < 1000; ++i) {
        EXPECT_EQ(high.limit(false), kAdmaDmaLimit);
        EXPECT_EQ(high.limit(true), kAdmaDmaLimit);
        EXPECT_FALSE(high.proofDue(false, 4096));
        EXPECT_FALSE(high.writeCheckDue(true, 4096));
        EXPECT_FALSE(high.checking(true));
    }
}

TEST(SwitchReleaseDma, FailedHighAddressQualificationKeepsTheBounceBoundary)
{
    SwitchAdmaHigh high;
    high.refuse();
    for (bool write : {false, true}) {
        EXPECT_EQ(high.limit(write), kAdma4GiB);
        EXPECT_FALSE(high.proofDue(write, 4096));
        EXPECT_FALSE(high.writeCheckDue(write, 4096));
        EXPECT_FALSE(high.checking(write));
    }
    EXPECT_FALSE(high.proved(4096));
    EXPECT_EQ(high.state, SwitchAdmaHigh::kOff);
}
