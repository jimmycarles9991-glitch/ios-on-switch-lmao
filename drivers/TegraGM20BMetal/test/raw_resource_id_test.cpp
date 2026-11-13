// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_resource_id.h"

namespace {

TEST(ResourceId, AnObjectWithNoHeaderHasNone)
{
    EXPECT_EQ(raw::textureResourceId(0), 0u);
    EXPECT_EQ(raw::samplerResourceId(0), 0u);
    EXPECT_EQ(raw::textureResourceId(1u << 20), 0u);
    EXPECT_EQ(raw::samplerResourceId(1u << 12), 0u);
}

TEST(ResourceId, ATexturesAndASamplersIdsMakeNvksBindlessHandle)
{
    const uint64_t texture = raw::textureResourceId(0x12345), sampler = raw::samplerResourceId(0x678);
    EXPECT_EQ(raw::bindlessHandle(texture, sampler), 0x678u << 20 | 0x12345u);
    EXPECT_EQ(texture & sampler & 0xffffffffu, 0u);
}

TEST(ResourceId, NoRawIdIsAnIdM2vsPoolsHandOut)
{
    for (uint32_t header : {1u, 2u, 15u, 16u, 4095u})
        for (uint64_t id : {raw::textureResourceId(header), raw::samplerResourceId(header)}) {
            EXPECT_NE(id, 0u);
            EXPECT_GE(id, uint64_t(1) << 32) << header;
        }
}

}  // namespace
