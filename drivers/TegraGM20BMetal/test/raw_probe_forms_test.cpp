// SPDX-License-Identifier: MIT
// raw/runtime/raw_probe_forms.h and raw_probe_ops.h on the host: today's form of every barrier is the word sequence the
// runtime emitted before the probes (so a debug build with no probe armed sends the same words), each relaxed form holds
// no method that idles the pipe by itself and is the planner's lowering, and the 2D engine's probe work, run through the
// 2D engine model (raw/fx/test/twod_sim.h), copies, scales and resolves as fx's mip steps do.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include "../raw/fx/fx_mip.h"
#include "../raw/fx/test/twod_sim.h"
#include "../raw/runtime/raw_probe_forms.h"
#include "../raw/runtime/raw_probe_ops.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"

namespace {

using raw::EntryForm;
using raw::PassForm;
using raw::ProbeEngine;
using raw::ProbeForms;
using raw::ProbeTaken;
using raw::SwitchForm;

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
    raw::Push p{buffer, buffer + 256};
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

ProbeForms forms(PassForm pass, EntryForm entry, SwitchForm switchForm)
{
    ProbeForms f;
    f.pass = pass;
    f.entry = entry;
    f.switchForm = switchForm;
    return f;
}

TEST(ProbeForms, ParsesEveryFormAndNothingElse)
{
    ProbeForms f;
    ASSERT_TRUE(raw::parseProbeForms("pass=pixel entry=planner switch=nowfi_noflush resolve=twod", f));
    EXPECT_EQ(f.pass, PassForm::PixelOrdered);
    EXPECT_EQ(f.entry, EntryForm::Planner);
    EXPECT_EQ(f.switchForm, SwitchForm::NoWfiNoFlush);
    EXPECT_TRUE(f.resolveTwoD);
    EXPECT_FALSE(f.keepsTiledFlush());
    ASSERT_TRUE(raw::parseProbeForms("", f));
    EXPECT_FALSE(f.any());
    ASSERT_TRUE(raw::parseProbeForms("pass=pixel_nowfi  entry=bare switch=none", f));
    EXPECT_EQ(f.pass, PassForm::PixelNoWfi);
    EXPECT_EQ(f.entry, EntryForm::Bare);
    EXPECT_EQ(f.switchForm, SwitchForm::None);
    EXPECT_FALSE(raw::parseProbeForms("pass=wfi", f));
    EXPECT_FALSE(raw::parseProbeForms("pass", f));
    EXPECT_FALSE(raw::parseProbeForms("barrier=none", f));
    ASSERT_TRUE(raw::parseProbeForms("index=8", f));
    EXPECT_EQ(f.indexLimit, 8u);
    EXPECT_TRUE(f.any());
    EXPECT_FALSE(raw::parseProbeForms("index=0", f));
    EXPECT_FALSE(raw::parseProbeForms("index=-4", f));
    EXPECT_FALSE(raw::parseProbeForms("index=", f));
}

TEST(ProbeForms, TodayIsWhatTheRuntimeEmittedBefore)
{
    const ProbeForms today;
    for (ProbeEngine before : {ProbeEngine::None, ProbeEngine::ThreeD, ProbeEngine::Copy, ProbeEngine::Compute}) {
        ProbeTaken taken;
        EXPECT_EQ(words([&](raw::Push &p) {
                      raw::probePassPrologue(p, today, before, taken);
                  }),
                  words([](raw::Push &p) {
                      raw::probe_forms_detail::barrier(p, switch_gm20b_pass_barrier, false);
                  }));
        EXPECT_EQ(words([&](raw::Push &p) {
                      raw::probeBlitOpening(p, today, before, taken);
                  }),
                  (std::vector<Method>{{0, kWfi, 0}}));
        const std::vector<Method> compute = {
            {1, kShaderCaches,
             NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) |
                 NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                 NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE)},
            {1, kTextureDataNoWfi, 0},
            {1, NVB1C0_INVALIDATE_SKED_CACHES, 0},
            {1, NVB1C0_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0},
            {1, NVB1C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0},
        };
        EXPECT_EQ(words([&](raw::Push &p) {
                      raw::probeComputeOpening(p, today, before, taken);
                  }),
                  compute);
        EXPECT_EQ(taken.passes + taken.entries + taken.firsts + taken.switches, 0u);
    }
}

TEST(ProbeForms, RelaxedPassBarriersAreThePlannersAndNeverIdle)
{
    const auto pass = [](PassForm f) {
        ProbeTaken taken;
        auto m = words([&](raw::Push &p) {
            raw::probePassPrologue(p, forms(f, EntryForm::Today, SwitchForm::Today), ProbeEngine::ThreeD, taken);
        });
        EXPECT_EQ(taken.passes, 1u);
        return m;
    };
    EXPECT_EQ(pass(PassForm::PixelOrdered), (std::vector<Method>{{0, kPixelBarrier, 0}, {0, kPixelOrdered, 0}}));
    EXPECT_EQ(pass(PassForm::PixelNoWfi), (std::vector<Method>{{0, kPixelBarrier, 0}, {0, kTextureDataNoWfi, 0}}));
    EXPECT_TRUE(pass(PassForm::None).empty());
    ProbeTaken taken;
    const ProbeForms pixel = forms(PassForm::PixelOrdered, EntryForm::Today, SwitchForm::Today);
    EXPECT_TRUE(idles(words([&](raw::Push &p) {
        raw::probePassPrologue(p, pixel, ProbeEngine::Copy, taken);
    })));
}

TEST(ProbeForms, RelaxedEngineChangesKeepTheInvalidatesAndNeverIdle)
{
    for (SwitchForm f : {SwitchForm::NoWfi, SwitchForm::NoWfiNoFlush, SwitchForm::None}) {
        const ProbeForms relaxed = forms(PassForm::Today, EntryForm::Today, f);
        ProbeTaken taken;
        const auto into3d = words([&](raw::Push &p) {
            raw::probePassPrologue(p, relaxed, ProbeEngine::Copy, taken);
        });
        const auto blit = words([&](raw::Push &p) {
            raw::probeBlitOpening(p, relaxed, ProbeEngine::ThreeD, taken);
        });
        const auto compute = words([&](raw::Push &p) {
            raw::probeComputeOpening(p, relaxed, ProbeEngine::ThreeD, taken);
        });
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
        EXPECT_TRUE(idles(words([&](raw::Push &p) {
            raw::probePassPrologue(p, relaxed, ProbeEngine::ThreeD, taken);
        })));
    }
}

TEST(ProbeForms, RelaxedEntriesInvalidateWhatTheJobReadsAndNeverIdle)
{
    for (EntryForm f : {EntryForm::Planner, EntryForm::Bare}) {
        uint32_t buffer[64];
        raw::Push p{buffer, buffer + 64};
        ProbeTaken taken;
        uint32_t *word = raw::probeEntry(p, f, taken);
        const auto m = decode(buffer, p.at);
        EXPECT_FALSE(idles(m));
        EXPECT_EQ(taken.entries, 1u);
        const uint32_t expect = f == EntryForm::Planner
                                    ? NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE) |
                                          NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE)
                                    : 0u;
        EXPECT_EQ(*word, expect);
        EXPECT_EQ(m.size(), f == EntryForm::Planner ? 4u : 3u);
        const bool texture = std::find(m.begin(), m.end(), Method{0, kTextureDataNoWfi, 0}) != m.end();
        EXPECT_EQ(texture, f == EntryForm::Planner);
        // the first encoder of a relaxed job opens with nothing
        const ProbeForms entry = forms(PassForm::Today, f, SwitchForm::Today);
        EXPECT_TRUE(words([&](raw::Push &q) {
                        raw::probePassPrologue(q, entry, ProbeEngine::None, taken);
                    }).empty());
        EXPECT_TRUE(words([&](raw::Push &q) {
                        raw::probeBlitOpening(q, entry, ProbeEngine::None, taken);
                    }).empty());
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
    raw::ProbeSurface ms{0x100000, 0xcf, 2 * w * 4, 2 * w, 2 * h, false, 0};
    raw::ProbeSurface into{0x200000, 0xcf, w * 4, w, h, false, 0};
    raw::ProbeOp op;
    ASSERT_TRUE(raw::probeResolveOp(ms, 1, 1, into, op));
    EXPECT_FALSE(raw::probeResolveOp(ms, 2, 1, into, op));
    ASSERT_TRUE(raw::probeResolveOp(ms, 1, 1, into, op));
    uint32_t buffer[128];
    raw::Push p{buffer, buffer + 128};
    raw::emitProbeOp(p, op);
    EXPECT_LE(size_t(p.at - buffer), raw::probeOpWords(op));
    twodsim::Sim sim;
    sim.map(ms.va, src);
    sim.map(into.va, dst);
    ASSERT_TRUE(sim.run(buffer, size_t(p.at - buffer))) << sim.error();
    EXPECT_EQ(sim.launches, 1u);
    std::vector<uint8_t> want(dst.size());
    raw::fx::referenceDownsample(grid.data(), 2 * w * 4, 2 * w, 2 * h, want.data(), w * 4, w, h, 4);
    EXPECT_EQ(dst, want);
}

TEST(ProbeOps, PointCopyAtOneToOneIsExact)
{
    constexpr uint32_t w = 24, h = 12;
    std::vector<uint8_t> src = pattern(w, h), dst(size_t(w) * h * 4, 0);
    raw::ProbeOp op;
    op.src = {0x100000, 0xcf, w * 4, w, h, false, 0};
    op.dst = {0x200000, 0xcf, w * 4, w, h, false, 0};
    op.dw = w;
    op.dh = h;
    uint32_t buffer[128];
    raw::Push p{buffer, buffer + 128};
    raw::emitProbeOp(p, op);
    twodsim::Sim sim;
    sim.map(op.src.va, src);
    sim.map(op.dst.va, dst);
    ASSERT_TRUE(sim.run(buffer, size_t(p.at - buffer))) << sim.error();
    EXPECT_EQ(dst, src);
}

TEST(ProbeOps, FillNamesItsRectangleAndColour)
{
    raw::ProbeOp op;
    op.kind = raw::ProbeOpKind::Fill;
    op.dst = {0x300000, 0xca, 512, 64, 32, true, 2};
    op.dx = 3, op.dy = 4, op.dw = 10, op.dh = 6;
    op.colorFormat = raw::kTwodColorF32x4;
    op.color[0] = 0x3f800000, op.color[3] = 0x3f000000;
    const auto m = words([&](raw::Push &p) {
        raw::emitProbeOp(p, op);
    });
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_SET_RENDER_SOLID_PRIM_COLOR0, 0x3f800000}), m.end());
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_RENDER_SOLID_PRIM_MODE, 4}), m.end());
    EXPECT_EQ(m.back(), (Method{3, NV902D_RENDER_SOLID_PRIM_POINT_Y(1), 10}));
    EXPECT_NE(std::find(m.begin(), m.end(), Method{3, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 13}), m.end());
    EXPECT_FALSE(idles(m));
    EXPECT_EQ(raw::probeTwodFormat(81), 0xd0u);
    EXPECT_EQ(raw::probeTwodFormat(92), 0xe0u);
    EXPECT_EQ(raw::probeTwodFormat(55), 0u);
}

TEST(ProbeOps, AfterAnOpTheTwoDEngineIdlesBeforeTheThreeDEngineAndTheTextureCacheIsInvalidated)
{
    const auto m = words([](raw::Push &p) {
        raw::probeOpAfter(p);
    });
    EXPECT_EQ(m, (std::vector<Method>{{3, NV902D_WAIT_FOR_IDLE, 0},
                                      {0, NVB197_WAIT_FOR_IDLE, 0},
                                      {0, NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0}}));
    EXPECT_LE(m.size() * 2, raw::kProbeBracketWords);
}

TEST(ProbeOps, ReferenceFillSetsTheDdxStateClipsToTheSurfaceAndUnclipsAfter)
{
    raw::ProbeOp op;
    op.kind = raw::ProbeOpKind::Fill;
    op.referenceFill = true;
    op.dst = {0x300000, 0xcf, 256, 64, 32, false, 0};
    op.dx = 1, op.dy = 1, op.dw = 62, op.dh = 30;
    op.colorFormat = 0xcf;
    op.color[0] = 0xff204060;
    uint32_t buffer[128];
    raw::Push p{buffer, buffer + 128};
    raw::emitProbeOp(p, op);
    EXPECT_LE(size_t(p.at - buffer), raw::probeOpWords(op));
    const auto m = words([&](raw::Push &q) {
        raw::emitProbeOp(q, op);
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
    for (const raw::ProbeOpKind kind : {raw::ProbeOpKind::Fill, raw::ProbeOpKind::Scale}) {
        raw::ProbeOp op;
        op.kind = kind;
        op.src = {0x100000, 0xcf, 256, 64, 32, false, 0};
        op.dst = {0x300000, 0xcf, 256, 64, 32, false, 0};
        op.dx = 1, op.dy = 1, op.dw = 62, op.dh = 30;
        op.colorFormat = 0xcf;
        op.clip[0] = 8, op.clip[1] = 4, op.clip[2] = 16, op.clip[3] = 10;
        uint32_t buffer[128];
        raw::Push p{buffer, buffer + 128};
        raw::emitProbeOp(p, op);
        EXPECT_LE(size_t(p.at - buffer), raw::probeOpWords(op));
        const auto m = words([&](raw::Push &q) {
            raw::emitProbeOp(q, op);
        });
        const auto at = [&](Method x) {
            return size_t(std::find(m.begin(), m.end(), x) - m.begin());
        };
        const size_t launch = kind == raw::ProbeOpKind::Fill ? at({3, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 1})
                                                             : at({3, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, 0});
        ASSERT_LT(launch, m.size());
        EXPECT_LT(at({3, NV902D_SET_CLIP_X0, 8}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_WIDTH, 16}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_HEIGHT, 10}), launch);
        EXPECT_LT(at({3, NV902D_SET_CLIP_ENABLE, 1}), launch);
        EXPECT_EQ(m.back(), (Method{3, NV902D_SET_CLIP_ENABLE, 0}));
    }
    raw::ProbeOp plain;
    plain.kind = raw::ProbeOpKind::Scale;
    const auto m = words([&](raw::Push &q) {
        raw::emitProbeOp(q, plain);
    });
    EXPECT_EQ(std::count_if(m.begin(), m.end(),
                            [](const Method &x) {
                                return std::get<1>(x) == NV902D_SET_CLIP_ENABLE;
                            }),
              0);
}

}  // namespace
