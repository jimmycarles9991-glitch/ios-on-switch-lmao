// SPDX-License-Identifier: MIT
// Engine rules over kits/SwitchGM20B:caps: the routes today's encode takes stay selectable and are chosen exactly as today,
// a fixed-function engine is tried before a shader pass, and a route no device run proved waits for its probe.
#include "../plan_engine.h"
#include "fixtures.h"
#include <gtest/gtest.h>

namespace raw::plan::test {
namespace {

ResourceFacts tex(uint16_t format, uint32_t flags = 0, uint8_t samples = 1, Layout layout = Layout::BlockLinear)
{
    ResourceFacts f;
    f.kind = ResourceFacts::Texture;
    f.format = format;
    f.flags = flags;
    f.samples = samples;
    f.layout = layout;
    f.levels = 4;
    return f;
}

ResourceFacts buf()
{
    ResourceFacts f;
    f.kind = ResourceFacts::Buffer;
    f.bytes = 4096;
    return f;
}

uint32_t probe(Probe p)
{
    return 1u << uint32_t(p);
}

TEST(Engine, ProductionRoutesSelectableProbedRoutesWait)
{
    const Caps caps = chipCaps();
    for (size_t i = 0; i < size_t(Rule::Count); ++i) {
        const RuleEvidence &e = evidence(Rule(i));
        EXPECT_TRUE(switch_gm20b_features[e.feature].present) << kRuleNames[i];
        EXPECT_EQ(caps.selectable(Rule(i)), e.probe == Probe::None) << kRuleNames[i];
    }
}

TEST(Engine, CopiesOnTheCopyEngine)
{
    const Caps caps = chipCaps();
    for (const auto &[a, b] : {std::pair{tex(80), tex(80)}, std::pair{buf(), tex(115)}, std::pair{tex(70), buf()},
                               std::pair{buf(), buf()}}) {
        const Choice c = copyEngine(a, b, caps);
        EXPECT_EQ(c.rule, Rule::CopyEngine);
        EXPECT_EQ(c.engine, Engine::Copy);
    }
    EXPECT_FALSE(copyEngine(tex(80), tex(115), caps).valid()) << "texels of different sizes";
}

TEST(Engine, CompressedCopyWaitsForItsProbe)
{
    const ResourceFacts compressed = tex(80, ResourceFacts::Compressed);
    EXPECT_FALSE(copyEngine(compressed, tex(80), chipCaps()).valid());
    EXPECT_EQ(copyEngine(compressed, tex(80), chipCaps(probe(Probe::CeCompressed))).rule, Rule::CopyEngineCompressed);
    const Choice twod = copyEngine(compressed, tex(80), chipCaps(probe(Probe::TwodScale)));
    EXPECT_EQ(twod.rule, Rule::CopyTwoD) << "through the texture unit, which reads compressed memory";
    EXPECT_EQ(twod.engine, Engine::TwoD);
}

TEST(Engine, FillsOnTheCopyEngine)
{
    EXPECT_EQ(fillEngine(buf(), chipCaps()).rule, Rule::FillCopyEngine);
    EXPECT_FALSE(fillEngine(tex(80), chipCaps()).valid());
}

TEST(Engine, ClearsOnThe3DEngineUntilTheTwoDFillIsProbed)
{
    const ResourceFacts pitch = tex(80, 0, 1, Layout::Pitch);
    EXPECT_EQ(clearEngine(pitch, chipCaps()).rule, Rule::ClearThreeD);
    EXPECT_EQ(clearEngine(pitch, chipCaps(probe(Probe::TwodFill))).rule, Rule::ClearTwoD);
    EXPECT_EQ(clearEngine(tex(80), chipCaps(probe(Probe::TwodFill))).rule, Rule::ClearThreeD) << "block-linear";
}

// Today: the pack's resolve state for every resolved format (RawMetalEncode.mm resolvePasses)
TEST(Engine, ResolvesAsToday)
{
    EXPECT_EQ(resolveEngine(tex(80, 0, 4), tex(80), chipCaps()).rule, Rule::ResolveDraw);
    EXPECT_EQ(resolveEngine(tex(80, 0, 4), tex(80), chipCaps(probe(Probe::TwodResolve))).rule, Rule::ResolveTwoD);
    EXPECT_FALSE(resolveEngine(tex(252, 0, 4), tex(252), chipCaps()).valid()) << "depth: no resolve state";
}

// Today: the 2D engine for fx::twodFormat's five formats, the pack's mip state for the rest (RMBlitEncoder generateMipmapsForTexture:)
TEST(Engine, MipmapsAsToday)
{
    for (uint16_t f : {80, 70, 115, 10, 30}) {
        const Choice c = mipEngine(tex(f, ResourceFacts::MipChain2D), chipCaps());
        EXPECT_EQ(c.rule, Rule::MipTwoD) << f;
        EXPECT_EQ(c.engine, Engine::TwoD) << f;
    }
    EXPECT_EQ(mipEngine(tex(81), chipCaps()).rule, Rule::MipDraw) << "sRGB: the 3D engine's passes";
    EXPECT_FALSE(mipEngine(tex(55), chipCaps()).valid()) << "no mip state, no 2D filtering";
}

TEST(Engine, FetchLadderTakesTheRecordsRung)
{
    Rule rule = Rule::Count;
    EXPECT_EQ(fetchRung(uint8_t(Rung::Interlock), chipCaps(), rule), Rung::Interlock);
    EXPECT_EQ(rule, Rule::FetchInterlock);
    EXPECT_EQ(fetchRung(uint8_t(Rung::Barrier), chipCaps(), rule), Rung::Barrier);
    EXPECT_EQ(rule, Rule::FetchBarrier);
    const uint8_t candidates = uint8_t(Rung::Interlock) | 0x10 | 0x20;
    EXPECT_EQ(fetchRung(candidates, chipCaps(), rule), Rung::Interlock) << "candidates wait for their probes";
    EXPECT_EQ(fetchRung(candidates, chipCaps(probe(Probe::IteratedBlend)), rule), Rung::Iterated);
    EXPECT_EQ(rule, Rule::FetchIterated);
    EXPECT_EQ(fetchRung(candidates, chipCaps(probe(Probe::BlendExact) | probe(Probe::IteratedBlend)), rule),
              Rung::Blend)
        << "fixed-function blend before the iterated blend";
}

}  // namespace
}  // namespace raw::plan::test
