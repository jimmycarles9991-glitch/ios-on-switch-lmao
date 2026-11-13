// SPDX-License-Identifier: MIT
// Validation: every recorded frame is plannable; each misuse Metal defines comes back as its class and problem.
#include "../plan_validate.h"
#include "fixtures.h"
#include <gtest/gtest.h>

namespace raw::plan::test {
namespace {

Finding check(const Recording &r, const Answers &answers = {}, const Caps &caps = chipCaps())
{
    return validate(r, build(r, caps), answers, ChannelState{});
}

struct Simple {
    Rec rec;
    uint32_t target, program;
    PassDesc desc;

    Simple()
    {
        target = rec.texture(80, 64, 64);
        program = rec.program(caState(1, {80}));
        desc = pass({colour(target, Load::Clear, Store::Store)}, 64, 64);
    }

    Recording one(uint32_t state = kNone, uint8_t indexSize = 0, uint64_t indexOffset = 0)
    {
        rec.render(desc);
        rec.draw(state == kNone ? rec.state(program, {}) : state, 6, indexSize);
        rec.r.draws.back().indexOffset = indexOffset;
        rec.end();
        return rec.r;
    }
};

struct OnePassForValidation {
    Recording r;

    OnePassForValidation()
    {
        Rec rec;
        const uint32_t standIn = rec.texture(115, 64, 64), xr = rec.texture(552, 64, 64, ResourceFacts::ExtendedRange);
        const uint32_t target = rec.texture(80, 64, 64);
        rec.r.resources[xr].standIn = standIn;
        rec.render(pass({colour(target, Load::Clear, Store::Store)}, 64, 64));
        rec.draw(rec.state(rec.program(caState(1, {80})), {texture(xr)}), 6, 0);
        rec.end();
        r = rec.r;
    }
};

TEST(Validate, RecordedFramesArePlannable)
{
    for (const Recording &r : {glassFrame0(), glassFrame1(), scrollFrame()})
        EXPECT_TRUE(check(r).plannable());
}

TEST(Validate, ClientErrors)
{
    {
        Simple s;
        s.desc.width = 128;
        EXPECT_EQ(check(s.one()).problem, Problem::RenderTargetSize);
    }
    {
        Simple s;
        s.program = s.rec.program(caState(2, {70}));
        EXPECT_EQ(check(s.one()).problem, Problem::PipelineColorFormat);
    }
    {
        Simple s;
        const Finding f = check(s.one(kNone, 4, 6));
        EXPECT_EQ(f.verdict, Verdict::ClientError);
        EXPECT_EQ(f.problem, Problem::IndexBufferOffset);
    }
    {
        Simple s;
        const uint32_t m = s.rec.texture(80, 64, 64, ResourceFacts::Memoryless);
        s.desc = pass({colour(m, Load::Load, Store::Store)}, 64, 64);
        EXPECT_EQ(check(s.one()).problem, Problem::MemorylessAccess);
    }
    {
        Simple s;
        ProgramFacts p = caState(3, {80});
        p.samples = 4;
        s.program = s.rec.program(p);
        EXPECT_EQ(check(s.one()).problem, Problem::PipelineSampleCount);
    }
}

TEST(Validate, QueryGated)
{
    Simple s;
    s.desc.layers = 2;
    const Recording r = s.one();
    EXPECT_EQ(check(r).verdict, Verdict::QueryGated);
    Answers yes;
    yes.layeredRendering = true;
    EXPECT_TRUE(check(r, yes).plannable());
}

TEST(Validate, RemainingGaps)
{
    {
        Simple s;
        s.rec.r.resources[s.target].flags |= ResourceFacts::RotatedPlane;
        EXPECT_EQ(check(s.one()).problem, Problem::RotatedTarget);
    }
    {
        Simple s;
        s.program = s.rec.program(caState(4, {80}, 1, Rung::Interlock));
        s.rec.r.programs[s.program].forms = ProgramFacts::Whole;
        const Finding f = check(s.one());
        EXPECT_EQ(f.verdict, Verdict::NoGpuRung);
        EXPECT_EQ(f.problem, Problem::FetchWithoutStorageForm);
    }
    {
        Rec rec;
        const uint32_t t = rec.texture(55, 64, 64, 0, 4);
        rec.blit();
        Transfer m;
        m.kind = Transfer::Mipmaps;
        m.destination = t;
        rec.transfer(m);
        rec.end();
        EXPECT_EQ(check(rec.r).problem, Problem::MipFormat);
    }
}

TEST(Validate, AConversionWithNoPackState)
{
    OnePassForValidation f;
    Caps caps = chipCaps();
    caps.own.has = [](const void *, OwnState s, uint16_t) {
        return s != OwnState::ExtendedRange;
    };
    EXPECT_EQ(check(f.r, {}, caps).problem, Problem::ConversionState);
}

TEST(Validate, LostChannelFailsTheCommandBuffer)
{
    const Recording r = glassFrame1();
    ChannelState lost;
    lost.lost = true;
    const Finding f = validate(r, build(r, chipCaps()), Answers{}, lost);
    EXPECT_EQ(f.verdict, Verdict::CommandBufferError);
    EXPECT_EQ(f.commandBufferError, 1) << "MTLCommandBufferErrorInternal";
}

}  // namespace
}  // namespace raw::plan::test
