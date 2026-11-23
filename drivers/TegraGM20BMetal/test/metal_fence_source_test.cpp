// SPDX-License-Identifier: MIT
// Guards on why an MTLFence needs no push of its own on the driver (TegraGM20BMetalFences.mm), which no host model runs: every
// encoder the driver opens begins with a drain or an engine change, which waits for the engine left behind
// (barrier_forms.h), and the raw encode emits nothing for a fence. When an encoder opens with neither, fences need a
// barrier.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

// Each opening's forms are barrier_forms.h's (probe_forms_test pins their words): today's drain, or at a change
// of engine the change itself
TEST(GM20BMetalFenceSource, EveryRawEncoderOpensWithADrainOrAnEngineChange)
{
    EXPECT_THAT(Body(SourceText("../blit.cpp"), "BlitEncoder *CommandBuffer::blit()"), HasSubstr("blitOpening("));
    EXPECT_THAT(Between(Body(SourceText("../pass_end.cpp"), "void RenderEncoder::end()"),
                        "// pass prologue:", "for (uint32_t j = 0;"),
                HasSubstr("passOpening("));
    // the compute encoder drains through the waiting INVALIDATE_SHADER_CACHES (compute_opening_test pins its methods)
    const std::string compute = SourceText("../compute.cpp");
    EXPECT_THAT(Body(compute, "ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev)"),
                HasSubstr("computeEncoderOpening("));
    EXPECT_THAT(Body(SourceText("../compute_opening.h"), "inline void computeOpening(Push &p)"),
                HasSubstr("p.m(NVB1C0_INVALIDATE_SHADER_CACHES, "));
    const std::string forms = SourceText("../barrier_forms.h");
    EXPECT_THAT(Body(forms, "constexpr bool switchDrainProven(BarrierEngine before, BarrierEngine next)"),
                Not(HasSubstr("BarrierEngine::None")));
    // a pass opens with the pixel barrier alone only where its uses are known, and a fence wait makes them unknown
    EXPECT_THAT(Body(SourceText("../pass_hazard.h"),
                     "constexpr DrainCause drainCause(const PassChain &chain, const PassUses &next)"),
                HasSubstr("if (!next.known)\n        return DrainCause::Unknown;"));
    EXPECT_THAT(SourceText("../TegraGM20BMetalPassUses.h"), HasSubstr("op == OpWaitFence"));
    // a job's first encoder opens with nothing behind the job before's end, which waits for idle as it releases
    EXPECT_THAT(Body(SourceText("../event_release.h"),
                     "inline void emitEventRelease(Push &p, uint64_t va, uint32_t payload, bool publish)"),
                HasSubstr("NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN)"));
}

TEST(GM20BMetalFenceSource, TheRawEncodeEmitsNothingForAFence)
{
    const std::string encode = SourceText("../TegraGM20BMetalRangeStep.mm");
    EXPECT_THAT(encode, Not(HasSubstr("case OpUpdateFence")));
    EXPECT_THAT(encode, Not(HasSubstr("case OpWaitFence")));
}

}  // namespace
