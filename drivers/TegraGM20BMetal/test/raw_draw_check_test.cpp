// SPDX-License-Identifier: MIT
// raw/runtime/raw_draw_check.h: a draw's check runs again after every bind that can change its answer (a planar or XR
// texture bound or replaced, a texture under a cubic sampler or of a state that writes textures, a cubic sampler bound or
// replaced, a buffer, a pipeline, a depth/stencil state, an opaque call) and after no other; the facade's binds and its
// check use the rule.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_draw_check.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

using raw::CheckedTexture;

constexpr CheckedTexture kPlain{}, kPlanar{true, false}, kXr{false, true};

TEST(DrawCheck, APlainTextureOverAPlainOneLeavesTheAnswer)
{
    EXPECT_FALSE(raw::textureBindChecks(kPlain, kPlain, false, false));
    EXPECT_FALSE(raw::samplerBindChecks(false, false));
}

TEST(DrawCheck, APlanarOrXrTextureBoundOrReplacedChecksAgain)
{
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlanar, false, false)) << "a constexpr sampler refuses it";
    EXPECT_TRUE(raw::textureBindChecks(kPlanar, kPlain, false, false)) << "the refusal it made goes";
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kXr, false, false)) << "its planes take a variant";
    EXPECT_TRUE(raw::textureBindChecks(kXr, kPlain, false, false));
}

TEST(DrawCheck, ATextureUnderACubicSamplerOrOfAStateThatWritesTexturesChecksAgain)
{
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlain, true, false));
    EXPECT_TRUE(raw::textureBindChecks(kPlain, kPlain, false, true));
}

TEST(DrawCheck, ACubicSamplerBoundOrReplacedChecksAgain)
{
    EXPECT_TRUE(raw::samplerBindChecks(false, true));
    EXPECT_TRUE(raw::samplerBindChecks(true, false));
}

// The facade: its check keeps its answer only at the same bind count, plan epoch and pipeline; its binds count by the
// rule, and every other bind its check reads counts always
TEST(DrawCheck, TheFacadesBindsAndCheckUseTheRule)
{
    const std::string encoder = SourceText("../raw/runtime/RawMetalRenderEncoder.mm"),
                      bindings = SourceText("../raw/runtime/RawMetalBindings.mm"),
                      strides = SourceText("../raw/runtime/RawMetalStrides.mm");
    const std::string check = Body(encoder, "- (void)checkDraw");
    EXPECT_THAT(check,
                HasSubstr("if (_checkedEligibility == _eligibility && _checkedEpoch == _cb->_plan.stateEpoch() &&"));
    EXPECT_THAT(check, HasSubstr("_checkedPipeline == _pipe->_raw)"));
    EXPECT_THAT(Body(encoder, "- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state"),
                HasSubstr("_eligibility++;"));
    EXPECT_THAT(Body(encoder, "- (void)setDepthStencilState:(id<MTLDepthStencilState>)s"),
                HasSubstr("_eligibility++;"));
    EXPECT_THAT(Body(bindings, "void rmBindTexture("), HasSubstr("raw::textureBindChecks("));
    EXPECT_THAT(Body(bindings, "void rmBindSampler("), HasSubstr("raw::samplerBindChecks("));
    EXPECT_THAT(Body(bindings, "- (void)buffer:(int)stage object:(id<MTLBuffer>)b offset:(NSUInteger)offset index:"),
                HasSubstr("_eligibility++;"));
    EXPECT_THAT(Body(bindings, "- (void)bufferOffset:(int)stage offset:(NSUInteger)offset index:(NSUInteger)index"),
                HasSubstr("_eligibility++;"));
    EXPECT_THAT(Body(strides, "- (void)setVertexBuffer:(id<MTLBuffer>)b"), HasSubstr("_eligibility++;"));
    EXPECT_THAT(Body(strides, "- (void)setVertexBufferOffset:(NSUInteger)offset"), HasSubstr("_eligibility++;"));
}

}  // namespace
