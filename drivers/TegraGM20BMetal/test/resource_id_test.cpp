// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../resource_id.h"

namespace {

TEST(ResourceId, AnObjectWithNoHeaderHasNone)
{
    EXPECT_EQ(gm20b::textureResourceId(0), 0u);
    EXPECT_EQ(gm20b::samplerResourceId(0), 0u);
    EXPECT_EQ(gm20b::textureResourceId(1u << 20), 0u);
    EXPECT_EQ(gm20b::samplerResourceId(1u << 12), 0u);
}

TEST(ResourceId, ATexturesAndASamplersIdsMakeNvksBindlessHandle)
{
    const uint64_t texture = gm20b::textureResourceId(0x12345), sampler = gm20b::samplerResourceId(0x678);
    EXPECT_EQ(gm20b::bindlessHandle(texture, sampler), 0x678u << 20 | 0x12345u);
    EXPECT_EQ(texture & sampler & 0xffffffffu, 0u);
}

TEST(ResourceId, NoRawIdIsAnIdM2vsPoolsHandOut)
{
    for (uint32_t header : {1u, 2u, 15u, 16u, 4095u})
        for (uint64_t id : {gm20b::textureResourceId(header), gm20b::samplerResourceId(header)}) {
            EXPECT_NE(id, 0u);
            EXPECT_GE(id, uint64_t(1) << 32) << header;
        }
}

}  // namespace
