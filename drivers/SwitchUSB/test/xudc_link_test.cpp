// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../XudcLink.h"

TEST(XudcLink, PortSpeedAndControlPacketMatchTheNegotiatedLink)
{
    for (uint32_t code = 0; code != 16; ++code) {
        const uint32_t status = (code << 10) | 1u | (1u << 21);
        const int expected = code == 1   ? xudc::kSpeedFull
                             : code == 3 ? xudc::kSpeedHigh
                             : code == 4 ? xudc::kSpeedSuper
                                         : xudc::kSpeedNone;
        EXPECT_EQ(xudc::portSpeed(status), expected);
        EXPECT_EQ(xudc::ep0Packet(expected), code == 4 ? 512u : 64u);
    }
}

TEST(XudcLink, BurstsMatchTheFamiliesSuperSpeedCompanions)
{
    EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedSuper, 2, 1024, 15), 15u);
    EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedHigh, 2, 512, 15), 0u);
    EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedFull, 2, 64, 15), 0u);
    for (const int periodic : {1, 3}) {
        EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedSuper, periodic, 1024, 15), 0u);
        EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedHigh, periodic, 1024 | (2u << 11), 15), 2u);
        EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedHigh, periodic, 1024 | (3u << 11), 15), 2u);
        EXPECT_EQ(xudc::endpointBurst(xudc::kSpeedFull, periodic, 1023, 15), 0u);
    }
}

TEST(XudcLink, BosSuperSpeedCapabilityDescribesTheSameSpeeds)
{
    const auto &cap = xudc::kSuperSpeedCapability;
    EXPECT_EQ(cap[0], sizeof(cap));
    EXPECT_EQ(cap[1], 16);
    EXPECT_EQ(cap[2], 3);
    const uint32_t supported = cap[4] | static_cast<uint32_t>(cap[5]) << 8;
    EXPECT_EQ(supported, (1u << xudc::kSpeedFull) | (1u << xudc::kSpeedHigh) | (1u << xudc::kSpeedSuper));
    EXPECT_EQ(cap[6], xudc::kSpeedFull);
}
