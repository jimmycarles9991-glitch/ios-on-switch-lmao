// SPDX-License-Identifier: MIT
// raw/runtime/raw_bound_texture.h: the textures a draw reads bound as texture arguments. A texture of the rotation SPI bound so
// sent its command buffer to m2v, which has no texture of a turned plane; Metal's own textures of it are framebuffer-only, and
// an M4 Max reads one bound anyway as its memory lies.
#include <gtest/gtest.h>

#include "../raw/runtime/raw_bound_texture.h"

namespace {

struct Texture {
    unsigned tic = 9;
    uint32_t format = 80;  // BGRA8Unorm
    uint8_t rotation = 0;
    const Texture *standIn = nullptr;
};

TEST(BoundTexture, ATurnedPlaneIsReadAsItLies)
{
    for (uint8_t turn = 1; turn <= 4; turn++) {
        Texture t;
        t.rotation = turn;
        EXPECT_TRUE(raw::drawsBound(t)) << int(turn);
    }
}

TEST(BoundTexture, AnExtendedRangeTextureIsReadThroughItsStandIn)
{
    Texture standIn, t;
    t.format = 554;  // BGR10_XR
    EXPECT_FALSE(raw::drawsBound(t));
    t.standIn = &standIn;
    EXPECT_TRUE(raw::drawsBound(t));
}

TEST(BoundTexture, ATextureWithNoHeaderIsNotRead)
{
    Texture t;
    t.tic = 0;
    EXPECT_FALSE(raw::drawsBound(t));
}

}  // namespace
