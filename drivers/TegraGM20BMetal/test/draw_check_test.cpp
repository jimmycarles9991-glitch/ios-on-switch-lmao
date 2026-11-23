// SPDX-License-Identifier: MIT
// draw_check.h: a draw's check runs again after every bind that can change its answer (a planar or XR
// texture bound or replaced, a texture under a cubic sampler or of a state that writes textures, a cubic sampler bound or
// replaced, a buffer, a pipeline, a depth/stencil state, an opaque call) and after no other.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/draw_check.h"

namespace {

using gm20b::CheckedTexture;

constexpr CheckedTexture kPlain{}, kPlanar{true, false}, kXr{false, true};

TEST(DrawCheck, APlainTextureOverAPlainOneLeavesTheAnswer)
{
    EXPECT_FALSE(gm20b::textureBindChecks(kPlain, kPlain, false, false));
}

TEST(DrawCheck, APlanarOrXrTextureBoundOrReplacedChecksAgain)
{
    EXPECT_TRUE(gm20b::textureBindChecks(kPlain, kPlanar, false, false)) << "a constexpr sampler refuses it";
    EXPECT_TRUE(gm20b::textureBindChecks(kPlanar, kPlain, false, false)) << "the refusal it made goes";
    EXPECT_TRUE(gm20b::textureBindChecks(kPlain, kXr, false, false)) << "its planes take a variant";
    EXPECT_TRUE(gm20b::textureBindChecks(kXr, kPlain, false, false));
}

TEST(DrawCheck, ATextureUnderACubicSamplerOrOfAStateThatWritesTexturesChecksAgain)
{
    EXPECT_TRUE(gm20b::textureBindChecks(kPlain, kPlain, true, false));
    EXPECT_TRUE(gm20b::textureBindChecks(kPlain, kPlain, false, true));
}

}  // namespace
