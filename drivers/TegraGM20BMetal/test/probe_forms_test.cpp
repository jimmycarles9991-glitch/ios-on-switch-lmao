// SPDX-License-Identifier: MIT
// probe_forms.h and probe_ops.h on the host: today's form of every barrier is the word sequence the
// runtime emitted before the probes (so a debug build with no probe armed sends the same words), each relaxed form holds
// no method that idles the pipe by itself and is the planner's lowering, and the 2D engine's probe work, run through the
// 2D engine model (fx/test/twod_sim.h), copies, scales and resolves as fx's mip steps do.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "../fx/fx_mip.h"
#include "../fx/test/twod_sim.h"
#include "../probe_forms.h"
#include "../probe_ops.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"

namespace {

using gm20b::EntryForm;
using gm20b::PassForm;
using gm20b::ProbeForms;
using gm20b::ProbeTaken;
using gm20b::SwitchForm;

using Method = std::tuple<uint32_t, uint32_t, uint32_t>;  // subchannel, method, value

std::vector<Method> decode(const uint32_t *at, const uint32_t *end)
{
    std::vector<Method> m;
    while (at < end) {
        const uint32_t h = *at++, op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       mthd = (h & 0xfff) << 2;
        if (op == 4) {
            m.push_back({subc, mthd, count});
            continue;
        }
        EXPECT_EQ(op, 1u);
        for (uint32_t i = 0; i < count; i++)
            m.push_back({subc, mthd + 4 * i, *at++});
    }
    return m;
}

template <typename Emit> std::vector<Method> words(Emit emit)
{
    uint32_t buffer[256];
    gm20b::Push p{buffer, buffer + 256};
    emit(p);
    return decode(buffer, p.at);
}

constexpr uint32_t kWfi = 0x0110, kShaderCaches = 0x021c, kTextureDataWfi = 0x1338, kTextureDataNoWfi = 0x1288,
                   kPixelOrdered = 0x0f74, kPixelBarrier = 0x0de0,
                   kShaderCachesNoWfi = NVB197_INVALIDATE_SHADER_CACHES_NO_WFI;

// Methods that drain the pipe on their own: WAIT_FOR_IDLE, INVALIDATE_SHADER_CACHES (it waits before it flushes) and the
// texture invalidate's waiting form, on the 3D and compute subchannels alike
bool idles(const std::vector<Method> &m)
{
    return std::any_of(m.begin(), m.end(), [](const Method &x) {
        const uint32_t subc = std::get<0>(x), mthd = std::get<1>(x);
        return subc <= 1 && (mthd == kWfi || mthd == kShaderCaches || mthd == kTextureDataWfi);
    });
}

using gm20b::BarrierEngine;
using gm20b::Opened;

constexpr BarrierEngine kEngines[] = {BarrierEngine::None, BarrierEngine::ThreeD, BarrierEngine::Copy,
                                      BarrierEngine::Compute};

std::vector<Method> passWords(PassForm pass, SwitchForm s, EntryForm e, BarrierEngine before, Opened *opened = nullptr)
{
    return words([&](gm20b::Push &p) {
        const Opened o = gm20b::passOpening(p, pass, s, e, before);
        if (opened)
            *opened = o;
    });
}

std::vector<Method> blitWords(SwitchForm s, EntryForm e, BarrierEngine before, Opened *opened = nullptr)
{
    return words([&](gm20b::Push &p) {
        const Opened o = gm20b::blitOpening(p, s, e, before);
        if (opened)
            *opened = o;
    });
}

std::vector<Method> computeWords(SwitchForm s, EntryForm e, BarrierEngine before, Opened *opened = nullptr)
{
    return words([&](gm20b::Push &p) {
        const Opened o = gm20b::computeEncoderOpening(p, s, e, before);
        if (opened)
            *opened = o;
    });
}

const std::vector<Method> kComputeToday = {
    {1, kShaderCaches,
     NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) | NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
         NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE)},
    {1, kTextureDataNoWfi, 0},
    {1, NVB1C0_INVALIDATE_SKED_CACHES, 0},
    {1, NVB1C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0},
    {1, NVB1C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0},
};

TEST(ProbeForms, ParsesEveryFormAndNothingElse)
{
    ProbeForms f;
    ASSERT_TRUE(gm20b::parseProbeForms("pass=pixel entry=planner switch=nowfi_noflush resolve=twod", f));
    EXPECT_EQ(f.pass, PassForm::PixelOrdered);
    EXPECT_EQ(f.entry, EntryForm::Planner);
    EXPECT_EQ(f.switchForm, SwitchForm::NoWfiNoFlush);
    EXPECT_TRUE(f.resolveTwoD);
    EXPECT_FALSE(gm20b::keepsTiledFlush(f.switchForm));
    ASSERT_TRUE(gm20b::parseProbeForms("", f));
    EXPECT_FALSE(f.any());
    EXPECT_EQ(f.pass, PassForm::Rule) << "no form named: production's rule";
    ASSERT_TRUE(gm20b::parseProbeForms("pass=today switch=today entry=today", f));
    EXPECT_TRUE(f.any()) << "today's drain forced";
    EXPECT_EQ(f.switchForm, SwitchForm::Today);
    ASSERT_TRUE(gm20b::parseProbeForms("entry=none", f));
    EXPECT_EQ(f.entry, EntryForm::None);
    ASSERT_TRUE(gm20b::parseProbeForms("pass=none  entry=bare switch=none", f));
    EXPECT_EQ(f.pass, PassForm::None);
    EXPECT_EQ(f.entry, EntryForm::Bare);
    EXPECT_EQ(f.switchForm, SwitchForm::None);
    EXPECT_FALSE(gm20b::parseProbeForms("pass=pixel_nowfi", f)) << "0x1288 after the pixel barrier failed on the chip";
    EXPECT_FALSE(gm20b::parseProbeForms("pass=wfi", f));
    EXPECT_FALSE(gm20b::parseProbeForms("pass", f));
    EXPECT_FALSE(gm20b::parseProbeForms("barrier=none", f));
    ASSERT_TRUE(gm20b::parseProbeForms("index=8", f));
    EXPECT_EQ(f.indexLimit, 8u);
    EXPECT_TRUE(f.any());
    EXPECT_FALSE(gm20b::parseProbeForms("index=0", f));
    EXPECT_FALSE(gm20b::parseProbeForms("index=-4", f));
    EXPECT_FALSE(gm20b::parseProbeForms("index=", f));
}

TEST(ProbeForms, TodayIsWhatTheRuntimeEmittedBefore)
{
    for (BarrierEngine before : kEngines) {
        Opened opened[3];
        EXPECT_EQ(passWords(PassForm::Today, SwitchForm::Today, EntryForm::Today, before, &opened[0]),
                  words([before](gm20b::Push &p) {
                      gm20b::barrier_forms_detail::barrier(p, switch_gm20b_pass_drain, before != BarrierEngine::ThreeD);
                  }));
        EXPECT_EQ(blitWords(SwitchForm::Today, EntryForm::Today, before, &opened[1]),
                  (std::vector<Method>{{0, kWfi, 0}}));
        EXPECT_EQ(computeWords(SwitchForm::Today, EntryForm::Today, before, &opened[2]), kComputeToday);
        for (Opened o : opened)
            EXPECT_EQ(o, Opened::Today);
    }
}

// Rule barrier.switch_drain: 3D to copy or 2D, copy or 2D to 3D, 3D to compute, each held in gpu.x1.subchannel_switch_
// drain's cells; every other change and every pass after a pass is today's drain
TEST(BarrierForms, TheSwitchRuleRelaxesOnlyTheProbedEngineChanges)
{
    using E = BarrierEngine;
    for (E before : kEngines) {
        const SwitchForm into3d = gm20b::switchFormFor(SwitchForm::Rule, before, E::ThreeD),
                         intoCopy = gm20b::switchFormFor(SwitchForm::Rule, before, E::Copy),
                         intoCompute = gm20b::switchFormFor(SwitchForm::Rule, before, E::Compute);
        EXPECT_EQ(into3d == SwitchForm::NoWfi, before == E::Copy);
        EXPECT_EQ(intoCopy == SwitchForm::NoWfi, before == E::ThreeD);
        EXPECT_EQ(intoCompute == SwitchForm::NoWfi, before == E::ThreeD);
        Opened o;
        const auto pass = passWords(PassForm::Today, into3d, EntryForm::Rule, before, &o);
        EXPECT_EQ(idles(pass), before != E::Copy) << int(before);
        EXPECT_EQ(o == Opened::Switch, before == E::Copy);
        const auto blit = blitWords(intoCopy, EntryForm::Rule, before, &o);
        EXPECT_EQ(blit.empty(), before == E::ThreeD);
        EXPECT_EQ(o == Opened::Switch, before == E::ThreeD);
        const auto compute = computeWords(intoCompute, EntryForm::Rule, before, &o);
        EXPECT_EQ(idles(compute), before != E::ThreeD);
        EXPECT_EQ(o == Opened::Switch, before == E::ThreeD);
    }
    EXPECT_EQ(gm20b::switchFormFor(SwitchForm::Today, BarrierEngine::Copy, BarrierEngine::ThreeD), SwitchForm::Today)
        << "a probe's forced drain";
    EXPECT_EQ(gm20b::entryFormFor(EntryForm::Rule), EntryForm::Planner);
    EXPECT_EQ(gm20b::entryFormFor(EntryForm::Today), EntryForm::Today) << "a probe's forced drain";
    EXPECT_EQ(gm20b::entryFormFor(EntryForm::None), EntryForm::None);
    EXPECT_TRUE(gm20b::keepsTiledFlush(SwitchForm::Rule));
    EXPECT_TRUE(gm20b::keepsTiledFlush(SwitchForm::NoWfi));
}

TEST(ProbeForms, RelaxedPassBarriersAreThePlannersAndNeverIdle)
{
    const auto pass = [](PassForm f) {
        Opened o;
        auto m = passWords(f, SwitchForm::Today, EntryForm::Today, BarrierEngine::ThreeD, &o);
        EXPECT_EQ(o, Opened::Pass);
        return m;
    };
    EXPECT_EQ(pass(PassForm::PixelOrdered), (std::vector<Method>{{0, kPixelBarrier, 0}, {0, kPixelOrdered, 0}}));
    EXPECT_TRUE(pass(PassForm::None).empty());
    EXPECT_TRUE(idles(passWords(PassForm::PixelOrdered, SwitchForm::Today, EntryForm::Today, BarrierEngine::Copy)));
}

// The pixel-ordered invalidate (0x0f74) follows the barrier in every form; 0x1288 there failed on the chip
TEST(ProbeForms, NoFormFollowsThePixelBarrierWithTheNoWfiInvalidate)
{
    for (PassForm pass : {PassForm::Rule, PassForm::Today, PassForm::PixelOrdered, PassForm::None})
        for (SwitchForm s :
             {SwitchForm::Rule, SwitchForm::Today, SwitchForm::NoWfi, SwitchForm::NoWfiNoFlush, SwitchForm::None})
            for (EntryForm e :
                 {EntryForm::Rule, EntryForm::Today, EntryForm::Planner, EntryForm::Bare, EntryForm::None})
                for (BarrierEngine before : kEngines) {
                    const auto m = passWords(pass, s, e, before);
                    for (size_t i = 0; i + 1 < m.size(); i++)
                        EXPECT_FALSE(std::get<1>(m[i]) == kPixelBarrier && std::get<1>(m[i + 1]) == kTextureDataNoWfi);
                }
}

TEST(ProbeForms, RelaxedEngineChangesKeepTheInvalidatesAndNeverIdle)
{
    for (SwitchForm f : {SwitchForm::NoWfi, SwitchForm::NoWfiNoFlush, SwitchForm::None}) {
        ProbeTaken taken;
        Opened o;
        const auto into3d = passWords(PassForm::Today, f, EntryForm::Today, BarrierEngine::Copy, &o);
        taken.count(o);
        const auto blit = blitWords(f, EntryForm::Today, BarrierEngine::ThreeD, &o);
        taken.count(o);
        const auto compute = computeWords(f, EntryForm::Today, BarrierEngine::ThreeD, &o);
        taken.count(o);
        EXPECT_EQ(taken.switches, 3u);
        EXPECT_FALSE(idles(into3d) || idles(blit) || idles(compute));
        EXPECT_TRUE(blit.empty());
        const bool invalidates = f != SwitchForm::None;
        EXPECT_EQ(into3d.size(), invalidates ? 2u : 0u);
        EXPECT_EQ(compute.size(), invalidates ? 5u : 3u);
        if (invalidates) {
            EXPECT_EQ(std::get<1>(into3d[0]), kShaderCachesNoWfi);
            EXPECT_EQ(into3d[1], (Method{0, kTextureDataNoWfi, 0}));
        }
        // a pass after a pass is the pass form's, today's here
        EXPECT_TRUE(idles(passWords(PassForm::Today, f, EntryForm::Today, BarrierEngine::ThreeD)));
    }
}

TEST(ProbeForms, RelaxedEntriesInvalidateWhatTheJobReadsAndNeverIdle)
{
    for (EntryForm f : {EntryForm::Planner, EntryForm::Bare, EntryForm::None}) {
        uint32_t buffer[64];
        gm20b::Push p{buffer, buffer + 64};
        ProbeTaken taken;
        uint32_t *word = gm20b::entryOpening(p, f);
        const auto m = decode(buffer, p.at);
        EXPECT_FALSE(idles(m));
        const uint32_t expect = f == EntryForm::Planner
                                    ? NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE) |
                                          NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE)
                                    : 0u;
        EXPECT_EQ(*word, expect) << "the word commit gives the INSTRUCTION bit where the heap grew";
        EXPECT_EQ(m.size(), f == EntryForm::Planner ? 4u : f == EntryForm::Bare ? 3u : 1u);
        const bool headers =
            std::find(m.begin(), m.end(), Method{0, NVB197_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0}) != m.end();
        EXPECT_EQ(headers, f != EntryForm::None) << "entry_none: every cache but the instructions keeps job 0's lines";
        const bool texture = std::find(m.begin(), m.end(), Method{0, kTextureDataNoWfi, 0}) != m.end();
        EXPECT_EQ(texture, f == EntryForm::Planner);
        // the first encoder of a relaxed job opens with nothing
        Opened o;
        EXPECT_TRUE(passWords(PassForm::Today, SwitchForm::Today, f, BarrierEngine::None, &o).empty());
        taken.count(o);
        EXPECT_TRUE(blitWords(SwitchForm::Today, f, BarrierEngine::None, &o).empty());
        taken.count(o);
        EXPECT_EQ(taken.firsts, 2u);
    }
}

// A BGRA8 image whose texel (x, y) holds bytes of x, y and their mix
std::vector<uint8_t> pattern(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *t = &out[(size_t(y) * w + x) * 4];
            t[0] = uint8_t(x * 7 + y * 3);
            t[1] = uint8_t(y * 11 + 5);
            t[2] = uint8_t((x ^ y) * 13);
            t[3] = uint8_t(255 - x);
        }
    return out;
}

TEST(ProbeOps, ResolveIsTheBoxOfEachPixelsSamples)
{
    constexpr uint32_t w = 16, h = 8;
    const std::vector<uint8_t> grid = pattern(2 * w, 2 * h);
    std::vector<uint8_t> src(grid), dst(size_t(w) * h * 4, 0xcd);
    gm20b::ProbeSurface ms{0x100000, 0xcf, 2 * w * 4, 2 * w, 2 * h, false, 0};
    gm20b::ProbeSurface into{0x200000, 0xcf, w * 4, w, h, false, 0};
    gm20b::ProbeOp op;
    ASSERT_TRUE(gm20b::probeResolveOp(ms, 1, 1, into, op));
    EXPECT_FALSE(gm20b::probeResolveOp(ms, 2, 1, into, op));
    ASSERT_TRUE(gm20b::probeResolveOp(ms, 1, 1, into, op));
    uint32_t buffer[128];
    gm20b::Push p{buffer, buffer + 128};
    gm20b::emitProbeOp(p, op);
    EXPECT_LE(size_t(p.at - buffer), gm20b::probeOpWords(op));
    twodsim::Sim sim;
    sim.map(ms.va, src);
    sim.map(into.va, dst);
    ASSERT_TRUE(sim.run(buffer, size_t(p.at - buffer))) << sim.error();
    EXPECT_EQ(sim.launches, 1u);
    std::vector<uint8_t> want(dst.size());
    gm20b::fx::referenceDownsample(grid.data(), 2 * w * 4, 2 * w, 2 * h, want.data(), w * 4, w, h, 4);
    EXPECT_EQ(dst, want);
}

TEST(ProbeOps, PointCopyAtOneToOneIsExact)
{
    constexpr uint32_t w = 24, h = 12;
    std::vector<uint8_t> src = pattern(w, h), dst(size_t(w) * h * 4, 0);
    gm20b::ProbeOp op;
    op.src = {0x100000, 0xcf, w * 4, w, h, false, 0};
    op.dst = {0x200000, 0xcf, w * 4, w, h, false, 0};
    op.dw = w;
    op.dh = h;
    uint32_t buffer[128];
    gm20b::Push p{buffer, buffer + 128};
    gm20b::emitProbeOp(p, op);
    twodsim::Sim sim;
    sim.map(op.src.va, src);
    sim.map(op.dst.va, dst);
    ASSERT_TRUE(sim.run(buffer, size_t(p.at - buffer))) << sim.error();
    EXPECT_EQ(dst, src);
}

TEST(ProbeOps, FillNamesItsRectangleAndColour)
{
    gm20b::ProbeOp op;
    op.kind = gm20b::ProbeOpKind::Fill;
    op.dst = {0x300000, 0xca, 512, 64, 32, true, 2};
    op.dx = 3, op.dy = 4, op.dw = 10, op.dh = 6;
    op.colorFormat = gm20b::kTwodColorF32x4;
    op.color[0] = 0x3f800000, op.color[3] = 0x3f000000;
    const auto m = words([&](gm20b::Push &p) {
        gm20b::emitProbeOp(p, op);
    });
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_SET_RENDER_SOLID_PRIM_COLOR0, 0x3f800000}), m.end());
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_RENDER_SOLID_PRIM_MODE, 4}), m.end());
    EXPECT_EQ(m.back(), (Method{3, NV902D_RENDER_SOLID_PRIM_POINT_Y(1), 10}));
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 13}), m.end());
    EXPECT_FALSE(idles(m));
    EXPECT_EQ(gm20b::probeTwodFormat(81), 0xd0u);
    EXPECT_EQ(gm20b::probeTwodFormat(92), 0xe0u);
    EXPECT_EQ(gm20b::probeTwodFormat(55), 0u);
}

TEST(ProbeOps, AfterAnOpTheTwoDEngineIdlesBeforeTheThreeDEngineAndTheTextureCacheIsInvalidated)
{
    const auto m = words([](gm20b::Push &p) {
        gm20b::probeOpAfter(p);
    });
    EXPECT_EQ(m, (std::vector<Method>{{3, NV902D_WAIT_FOR_IDLE, 0},
                                      {0, NVB197_WAIT_FOR_IDLE, 0},
                                      {0, NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0}}));
    EXPECT_LE(m.size() * 2, gm20b::kProbeBracketWords);
}

TEST(ProbeOps, ReferenceFillSetsTheDdxStateClipsToTheSurfaceAndUnclipsAfter)
{
    gm20b::ProbeOp op;
    op.kind = gm20b::ProbeOpKind::Fill;
    op.referenceFill = true;
    op.dst = {0x300000, 0xcf, 256, 64, 32, false, 0};
    op.dx = 1, op.dy = 1, op.dw = 62, op.dh = 30;
    op.colorFormat = 0xcf;
    op.color[0] = 0xff204060;
    uint32_t buffer[128];
    gm20b::Push p{buffer, buffer + 128};
    gm20b::emitProbeOp(p, op);
    EXPECT_LE(size_t(p.at - buffer), gm20b::probeOpWords(op));
    const auto m = words([&](gm20b::Push &q) {
        gm20b::emitProbeOp(q, op);
    });
    const auto at = [&](Method x) {
        return size_t(std::find(m.begin(), m.end(), x) - m.begin());
    };
    const size_t mode = at({3, NV902D_RENDER_SOLID_PRIM_MODE, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS});
    ASSERT_LT(mode, m.size());
    EXPECT_LT(at({3, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY}), mode);
    EXPECT_LT(at({3, NV902D_SET_CLIP_ENABLE, 1}), mode);
    EXPECT_LT(at({3, NV902D_SET_CLIP_WIDTH, 64}), mode);
    EXPECT_LT(at({3, NV902D_SET_CLIP_HEIGHT, 32}), mode);
    EXPECT_EQ(at({3, NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT, 0xcf}), mode + 1);
    EXPECT_EQ(at({3, NV902D_SET_RENDER_SOLID_PRIM_COLOR, 0xff204060}), mode + 2);
    EXPECT_EQ(m.back(), (Method{3, NV902D_SET_CLIP_ENABLE, 0}));
    EXPECT_EQ(at({3, NV902D_SET_RENDER_SOLID_PRIM_COLOR0, 0}), m.size());
}

TEST(ProbeOps, AClipRectangleBracketsTheOperationItClips)
{
    for (const gm20b::ProbeOpKind kind : {gm20b::ProbeOpKind::Fill, gm20b::ProbeOpKind::Scale}) {
        gm20b::ProbeOp op;
        op.kind = kind;
        op.src = {0x100000, 0xcf, 256, 64, 32, false, 0};
        op.dst = {0x300000, 0xcf, 256, 64, 32, false, 0};
        op.dx = 1, op.dy = 1, op.dw = 62, op.dh = 30;
        op.colorFormat = 0xcf;
        op.clip[0] = 8, op.clip[1] = 4, op.clip[2] = 16, op.clip[3] = 10;
        uint32_t buffer[128];
        gm20b::Push p{buffer, buffer + 128};
        gm20b::emitProbeOp(p, op);
        EXPECT_LE(size_t(p.at - buffer), gm20b::probeOpWords(op));
        const auto m = words([&](gm20b::Push &q) {
            gm20b::emitProbeOp(q, op);
        });
        const auto at = [&](Method x) {
            return size_t(std::find(m.begin(), m.end(), x) - m.begin());
        };
        const size_t launch = kind == gm20b::ProbeOpKind::Fill ? at({3, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 1})
                                                               : at({3, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, 0});
        ASSERT_LT(launch, m.size());
        EXPECT_LT(at({3, NV902D_SET_CLIP_X0, 8}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_WIDTH, 16}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_HEIGHT, 10}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_ENABLE, 1}), launch);
        EXPECT_EQ(m.back(), (Method{3, NV902D_SET_CLIP_ENABLE, 0}));
    }
    gm20b::ProbeOp plain;
    plain.kind = gm20b::ProbeOpKind::Scale;
    const auto m = words([&](gm20b::Push &q) {
        gm20b::emitProbeOp(q, plain);
    });
    EXPECT_EQ(std::count_if(m.begin(), m.end(),
                            [](const Method &x) {
                                return std::get<1>(x) == NV902D_SET_CLIP_ENABLE;
                            }),
              0);
}

TEST(ProbeOps, ARopIsSetBeforeTheLaunchWithTheDdxsPatternAndSrccopyComesBackAfter)
{
    for (const gm20b::ProbeOpKind kind : {gm20b::ProbeOpKind::Fill, gm20b::ProbeOpKind::Scale}) {
        gm20b::ProbeOp op;
        op.kind = kind;
        op.src = {0x100000, 0xcf, 256, 64, 32, false, 0};
        op.dst = {0x300000, 0xcf, 256, 64, 32, false, 0};
        op.dw = 64, op.dh = 32;
        op.colorFormat = 0xcf;
        op.clip[0] = 8, op.clip[1] = 4, op.clip[2] = 16, op.clip[3] = 10;
        op.rop = kind == gm20b::ProbeOpKind::Fill ? 0x55 : 0x33;
        uint32_t buffer[128];
        gm20b::Push p{buffer, buffer + 128};
        gm20b::emitProbeOp(p, op);
        EXPECT_LE(size_t(p.at - buffer), gm20b::probeOpWords(op));
        const auto m = words([&](gm20b::Push &q) {
            gm20b::emitProbeOp(q, op);
        });
        const auto at = [&](Method x) {
            return size_t(std::find(m.begin(), m.end(), x) - m.begin());
        };
        const size_t launch = kind == gm20b::ProbeOpKind::Fill ? at({3, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 0})
                                                               : at({3, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, 0});
        ASSERT_LT(launch, m.size());
        EXPECT_LT(at({3, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_ROP}), launch);
        EXPECT_LT(at({3, NV902D_SET_ROP, op.rop}), launch);
        EXPECT_LT(
            at({3, NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT, NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT_V_A8R8G8B8}),
            launch);
        EXPECT_LT(at({3, NV902D_SET_MONOCHROME_PATTERN1, ~0u}), launch);
        EXPECT_EQ(m.back(), (Method{3, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY}));
    }
    gm20b::ProbeOp plain;
    plain.kind = gm20b::ProbeOpKind::Scale;
    const auto m = words([&](gm20b::Push &q) {
        gm20b::emitProbeOp(q, plain);
    });
    EXPECT_EQ(std::count_if(m.begin(), m.end(),
                            [](const Method &x) {
                                return std::get<1>(x) == NV902D_SET_OPERATION;
                            }),
              0);
}

}  // namespace
