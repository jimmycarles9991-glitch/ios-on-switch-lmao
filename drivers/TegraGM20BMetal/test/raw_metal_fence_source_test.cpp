// SPDX-License-Identifier: MIT
// Guards on why an MTLFence needs no push of its own on the raw path (RawMetalFences.mm), which no host model runs: every
// encoder the raw path opens begins with a drain or an engine change, which waits for the engine left behind
// (raw_barrier_forms.h), and the raw encode emits nothing for a fence. When an encoder opens with neither, fences need a
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

// Each opening's forms are raw_barrier_forms.h's (raw_probe_forms_test pins their words): today's drain, or at a change
// of engine the change itself
TEST(RawMetalFenceSource, EveryRawEncoderOpensWithADrainOrAnEngineChange)
{
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    EXPECT_THAT(Body(gpu, "BlitEncoder *CommandBuffer::blit()"), HasSubstr("blitOpening("));
    EXPECT_THAT(Between(Body(gpu, "void RenderEncoder::end()"), "// pass prologue:", "for (uint32_t j = 0;"),
                HasSubstr("passOpening("));
    // the compute encoder drains through the waiting INVALIDATE_SHADER_CACHES (raw_compute_opening_test pins its methods)
    const std::string compute = SourceText("../raw/runtime/raw_compute.cpp");
    EXPECT_THAT(Body(compute, "ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev)"),
                HasSubstr("computeEncoderOpening("));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_compute_opening.h"), "inline void computeOpening(Push &p)"),
                HasSubstr("p.m(NVB1C0_INVALIDATE_SHADER_CACHES, "));
    const std::string forms = SourceText("../raw/runtime/raw_barrier_forms.h");
    EXPECT_THAT(Body(forms, "constexpr bool switchDrainProven(BarrierEngine before, BarrierEngine next)"),
                Not(HasSubstr("BarrierEngine::None")));
    // a pass opens with the pixel barrier alone only where its uses are known, and a fence wait makes them unknown
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_pass_hazard.h"),
                     "constexpr bool pixelBarrierHolds(const PassChain &chain, const PassUses &next)"),
                HasSubstr("!next.known"));
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalPassUses.h"), HasSubstr("op == OpWaitFence"));
    // a job's first encoder opens with nothing behind the job before's end, which waits for idle as it releases
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_event_release.h"),
                     "inline void emitEventRelease(Push &p, uint64_t va, uint32_t payload, bool publish)"),
                HasSubstr("NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN)"));
}

TEST(RawMetalFenceSource, TheRawEncodeEmitsNothingForAFence)
{
    const std::string encode = SourceText("../raw/runtime/RawMetalRangeStep.mm");
    EXPECT_THAT(encode, Not(HasSubstr("case OpUpdateFence")));
    EXPECT_THAT(encode, Not(HasSubstr("case OpWaitFence")));
}

}  // namespace
