// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../depth_stencil_state.h"

namespace {
using gm20b::DepthStencilDesc;
using gm20b::DepthStencilRegs;
using gm20b::DepthStencilState;
using gm20b::StencilFace;

enum Reg : uint32_t {
    DepthTest,
    DepthWrite,
    DepthFunc,
    StencilTest,
    FrontFail,
    FrontDepthFail,
    FrontPass,
    FrontFunc,
    FrontRef,
    FrontFuncMask,
    FrontMask,
    BackFail,
    BackDepthFail,
    BackPass,
    BackFunc,
    BackRef,
    BackMask,
    BackFuncMask,
};

bool same(const DepthStencilRegs &a, const DepthStencilRegs &b)
{
    return !memcmp(a.v, b.v, sizeof a.v);
}

DepthStencilDesc varied(uint32_t seed)
{
    DepthStencilDesc d;
    d.depthCompare = uint8_t(seed & 7);
    d.depthWrite = seed & 8;
    StencilFace *faces[2] = {&d.front, &d.back};
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t s = seed >> (4 + 12 * i);
        faces[i]->compare = uint8_t(s & 7);
        faces[i]->fail = uint8_t((s >> 3) & 7);
        faces[i]->depthFail = uint8_t((s >> 6) & 7);
        faces[i]->pass = uint8_t((s >> 9) & 7);
        faces[i]->readMask = 0x1234500u | (s & 0xff);
        faces[i]->writeMask = 0xabcde00u | ((s >> 2) & 0xff);
    }
    return d;
}

TEST(DepthStencilState, PrecomputedWordsAreTheDrawsDerivationForEveryAspectAndReference)
{
    const uint32_t refs[] = {0, 1, 0x5a, 0xff, 0x100, 0xffffffffu};
    for (uint32_t seed = 0; seed < (1u << 16); seed += 97) {
        const DepthStencilDesc d = varied(seed * 2654435761u);
        const DepthStencilState state(d);
        for (uint32_t aspects = 0; aspects < 4; aspects++)
            for (uint32_t front : refs)
                for (uint32_t back : refs) {
                    const bool depth = aspects & 1, stencil = aspects & 2;
                    EXPECT_TRUE(same(state.regs(depth, stencil, front, back),
                                     gm20b::depthStencilRegs(d, front, back, depth, stencil)))
                        << "seed " << seed << " aspects " << aspects << " refs " << front << "/" << back;
                }
    }
}

TEST(DepthStencilState, CompareFunctionsAreTheClassHeadersOglValues)
{
    const uint32_t ogl[8] = {
        NVB197_SET_DEPTH_FUNC_V_OGL_NEVER,  NVB197_SET_DEPTH_FUNC_V_OGL_LESS,    NVB197_SET_DEPTH_FUNC_V_OGL_EQUAL,
        NVB197_SET_DEPTH_FUNC_V_OGL_LEQUAL, NVB197_SET_DEPTH_FUNC_V_OGL_GREATER, NVB197_SET_DEPTH_FUNC_V_OGL_NOTEQUAL,
        NVB197_SET_DEPTH_FUNC_V_OGL_GEQUAL, NVB197_SET_DEPTH_FUNC_V_OGL_ALWAYS,
    };
    for (uint8_t f = 0; f < 8; f++) {
        DepthStencilDesc d;
        d.depthCompare = f;
        d.front.compare = f;
        d.back.compare = f;
        const DepthStencilRegs r = DepthStencilState(d).regs(true, true, 0, 0);
        EXPECT_EQ(r.v[DepthFunc], ogl[f]);
        EXPECT_EQ(r.v[FrontFunc], ogl[f]);
        EXPECT_EQ(r.v[BackFunc], ogl[f]);
        EXPECT_EQ(ogl[f], NVB197_SET_STENCIL_FUNC_V_OGL_NEVER + f);
        EXPECT_EQ(ogl[f], NVB197_SET_BACK_STENCIL_FUNC_V_OGL_NEVER + f);
    }
}

TEST(DepthStencilState, StencilOperationsAreTheClassHeadersD3dValues)
{
    // MTLStencilOperation: Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap
    const uint32_t d3d[8] = {
        NVB197_SET_STENCIL_OP_FAIL_V_D3D_KEEP,    NVB197_SET_STENCIL_OP_FAIL_V_D3D_ZERO,
        NVB197_SET_STENCIL_OP_FAIL_V_D3D_REPLACE, NVB197_SET_STENCIL_OP_FAIL_V_D3D_INCRSAT,
        NVB197_SET_STENCIL_OP_FAIL_V_D3D_DECRSAT, NVB197_SET_STENCIL_OP_FAIL_V_D3D_INVERT,
        NVB197_SET_STENCIL_OP_FAIL_V_D3D_INCR,    NVB197_SET_STENCIL_OP_FAIL_V_D3D_DECR,
    };
    for (uint8_t op = 0; op < 8; op++) {
        DepthStencilDesc d;
        d.front = {7, op, uint8_t((op + 1) & 7), uint8_t((op + 2) & 7), ~0u, ~0u};
        d.back = {7, uint8_t((op + 3) & 7), op, uint8_t((op + 5) & 7), ~0u, ~0u};
        const DepthStencilRegs r = DepthStencilState(d).regs(true, true, 0, 0);
        EXPECT_EQ(r.v[FrontFail], d3d[op]);
        EXPECT_EQ(r.v[FrontDepthFail], d3d[(op + 1) & 7]);
        EXPECT_EQ(r.v[FrontPass], d3d[(op + 2) & 7]);
        EXPECT_EQ(r.v[BackFail], d3d[(op + 3) & 7]);
        EXPECT_EQ(r.v[BackDepthFail], d3d[op]);
        EXPECT_EQ(r.v[BackPass], d3d[(op + 5) & 7]);
    }
}

TEST(DepthStencilState, FacesKeepTheirOwnMasksAndReferences)
{
    DepthStencilDesc d;
    d.front = {2, 0, 0, 2, 0x1f0f, 0x2c3c};
    d.back = {5, 1, 0, 0, 0x0e, 0x7d};
    const DepthStencilRegs r = DepthStencilState(d).regs(true, true, 0x5a, 0x1c3);
    EXPECT_EQ(r.v[StencilTest], 1u);
    EXPECT_EQ(r.v[FrontRef], 0x5au);
    EXPECT_EQ(r.v[FrontFuncMask], 0x0fu);
    EXPECT_EQ(r.v[FrontMask], 0x3cu);
    EXPECT_EQ(r.v[BackRef], 0xffu);
    EXPECT_EQ(r.v[BackFuncMask], 0x0eu);
    EXPECT_EQ(r.v[BackMask], 0x7du);
}

TEST(DepthStencilState, EnablesNeedTheirAspect)
{
    DepthStencilDesc d;
    d.depthCompare = 1;
    d.depthWrite = true;
    d.front.pass = 2;
    const DepthStencilState state(d);
    for (uint32_t aspects = 0; aspects < 4; aspects++) {
        const DepthStencilRegs r = state.regs(aspects & 1, aspects & 2, 1, 1);
        EXPECT_EQ(r.v[DepthTest], aspects & 1);
        EXPECT_EQ(r.v[DepthWrite], aspects & 1);
        EXPECT_EQ(r.v[StencilTest], (aspects >> 1) & 1);
    }
    const DepthStencilRegs none = DepthStencilState().regs(true, true, 0, 0);
    EXPECT_EQ(none.v[DepthTest], 0u);
    EXPECT_EQ(none.v[StencilTest], 0u);
    EXPECT_EQ(none.v[DepthFunc], uint32_t(NVB197_SET_DEPTH_FUNC_V_OGL_ALWAYS));
}

TEST(DepthStencilState, MetalsDefaultIsOneSharedState)
{
    EXPECT_EQ(&DepthStencilState::metalDefault(), &DepthStencilState::metalDefault());
    EXPECT_TRUE(same(DepthStencilState::metalDefault().regs(true, true, 3, 4),
                     gm20b::depthStencilRegs(DepthStencilDesc(), 3, 4, true, true)));
}

// A segment word as the class decodes it: an immediate method (SEND_INC_IMMD) and its value
struct Immediate {
    uint32_t method, value;
};

Immediate decode(uint32_t word)
{
    EXPECT_EQ(word >> 29, 4u);
    EXPECT_EQ((word >> 13) & 7, 0u);
    return {(word & 0x1fff) << 2, (word >> 16) & 0x1fff};
}

TEST(DepthStencilState, BlockIsEveryRegisterButTheReferencesAsImmediateMethods)
{
    const uint32_t refs[] = {0, 0x5a, 0xff, 0x100};
    for (uint32_t seed = 0; seed < (1u << 16); seed += 211) {
        const DepthStencilDesc d = varied(seed * 2654435761u);
        const DepthStencilState state(d);
        for (uint32_t aspects = 0; aspects < 4; aspects++)
            for (uint32_t front : refs)
                for (uint32_t back : refs) {
                    const bool depth = aspects & 1, stencil = aspects & 2;
                    const DepthStencilRegs want = state.regs(depth, stencil, front, back);
                    uint32_t words[gm20b::kDepthStencilBlockWords + gm20b::kStencilReferenceWords];
                    memcpy(words, state.block(depth, stencil), sizeof(gm20b::DepthStencilBlock));
                    gm20b::Push p{words + gm20b::kDepthStencilBlockWords, words + sizeof words / 4};
                    gm20b::stencilReferenceWords(p, front, back);
                    ASSERT_EQ(p.at, p.end);
                    DepthStencilRegs got;
                    bool seen[gm20b::kDepthStencilRegs] = {};
                    for (uint32_t w : words) {
                        const Immediate m = decode(w);
                        const auto *at = std::find(std::begin(gm20b::kDepthStencilMethods),
                                                   std::end(gm20b::kDepthStencilMethods), m.method);
                        ASSERT_NE(at, std::end(gm20b::kDepthStencilMethods)) << std::hex << m.method;
                        const size_t i = size_t(at - std::begin(gm20b::kDepthStencilMethods));
                        EXPECT_FALSE(seen[i]);
                        seen[i] = true;
                        got.v[i] = m.value;
                    }
                    EXPECT_TRUE(same(got, want)) << "seed " << seed << " aspects " << aspects;
                }
    }
}

TEST(DepthStencilState, BlockLeavesTheReferencesToTheDraw)
{
    const DepthStencilState state(varied(0x9e3779b9u));
    for (uint32_t w : state.block(true, true)) {
        const uint32_t method = decode(w).method;
        EXPECT_NE(method, uint32_t(NVB197_SET_STENCIL_FUNC_REF));
        EXPECT_NE(method, uint32_t(NVB197_SET_BACK_STENCIL_FUNC_REF));
    }
}

TEST(DepthStencilState, SegmentWordsAreEveryRegisterButTheReferencesAsImmediateMethods)
{
    const uint32_t refs[] = {0, 0x5a, 0xff, 0x100};
    for (uint32_t seed = 0; seed < (1u << 16); seed += 211) {
        const DepthStencilDesc d = varied(seed * 2654435761u);
        const DepthStencilState state(d);
        for (uint32_t aspects = 0; aspects < 4; aspects++)
            for (uint32_t front : refs)
                for (uint32_t back : refs) {
                    const bool depth = aspects & 1, stencil = aspects & 2;
                    const DepthStencilRegs want = state.regs(depth, stencil, front, back);
                    uint32_t words[gm20b::kDepthStencilRegs];
                    ASSERT_EQ(gm20b::depthStencilStateWords(want, words), gm20b::kDepthStencilRegs - 2);
                    DepthStencilRegs got;
                    bool seen[gm20b::kDepthStencilRegs] = {};
                    for (const uint32_t reference : {gm20b::kFrontStencilReference, gm20b::kBackStencilReference}) {
                        got.v[reference] = want.v[reference];
                        seen[reference] = true;
                    }
                    for (uint32_t w = 0; w < gm20b::kDepthStencilRegs - 2; w++) {
                        const Immediate m = decode(words[w]);
                        const auto *at = std::find(std::begin(gm20b::kDepthStencilMethods),
                                                   std::end(gm20b::kDepthStencilMethods), m.method);
                        ASSERT_NE(at, std::end(gm20b::kDepthStencilMethods)) << std::hex << m.method;
                        const size_t i = size_t(at - std::begin(gm20b::kDepthStencilMethods));
                        EXPECT_FALSE(seen[i]);
                        seen[i] = true;
                        got.v[i] = m.value;
                    }
                    EXPECT_TRUE(same(got, want)) << "seed " << seed << " aspects " << aspects;
                }
    }
}

TEST(DepthStencilState, SegmentWordsLeaveTheReferencesToTheDraw)
{
    const DepthStencilState state(varied(0x9e3779b9u));
    uint32_t words[gm20b::kDepthStencilRegs];
    const uint32_t n = gm20b::depthStencilStateWords(state.regs(true, true, 0x5a, 0xa5), words);
    for (uint32_t w = 0; w < n; w++) {
        const uint32_t method = decode(words[w]).method;
        EXPECT_NE(method, uint32_t(NVB197_SET_STENCIL_FUNC_REF));
        EXPECT_NE(method, uint32_t(NVB197_SET_BACK_STENCIL_FUNC_REF));
    }
}

TEST(DepthStencilState, HiddenZetaForEveryCompareButAlwaysAndForStencilTests)
{
    for (uint8_t f = 0; f < 8; f++)
        for (bool write : {false, true}) {
            DepthStencilDesc d;
            d.depthCompare = f;
            d.depthWrite = write;
            EXPECT_EQ(gm20b::testsHiddenZeta(d), f != 7) << int(f) << " write " << write;
        }
    DepthStencilDesc stencil;
    stencil.back.compare = 2;
    EXPECT_TRUE(gm20b::testsHiddenZeta(stencil));
    DepthStencilDesc replaces;
    replaces.front.pass = 2;
    EXPECT_TRUE(gm20b::testsHiddenZeta(replaces));
    DepthStencilDesc failOnly;
    failOnly.front.fail = 2;  // a compare of Always never fails
    EXPECT_FALSE(gm20b::testsHiddenZeta(failOnly));
}

// Boot 21 metal.native.hidden_depth: a draw recorded with a depth test in a pass of no depth attachment was refused
// (enc.ds a=0 b=0x100000000) though the encode binds the pass's hidden zeta for it
TEST(DepthStencilState, ADrawThatTestsAHiddenZetaFitsAsRecordedAndAsEncoded)
{
    DepthStencilDesc less;
    less.depthCompare = 1;
    DepthStencilDesc stencil;
    stencil.front.compare = 5;
    for (const DepthStencilDesc &d : {less, stencil}) {
        EXPECT_TRUE(gm20b::recordedDepthStencilFits(0, 0, d));
        EXPECT_TRUE(gm20b::depthStencilFits(0, 0, true, d)) << "the hidden zeta bound";
        EXPECT_FALSE(gm20b::depthStencilFits(0, 0, false, d)) << "no zeta to test against";
    }
    const DepthStencilDesc always{};
    EXPECT_TRUE(gm20b::depthStencilFits(0, 0, false, always)) << "Always with no stencil test tests nothing";
    for (const DepthStencilDesc &d : {less, always}) {
        EXPECT_FALSE(gm20b::recordedDepthStencilFits(0, 252, d)) << "a state made for no attachment in a pass with one";
        EXPECT_FALSE(gm20b::recordedDepthStencilFits(260, 252, d));
        EXPECT_TRUE(gm20b::recordedDepthStencilFits(252, 252, d));
    }
}

// What an M4 Max stores for each clear depth (macOS 27.2, read back by a blit): a NaN of either sign the quiet NaN in a float
// depth and 0 in Depth16Unorm, every other value clamped to [0, 1]
TEST(DepthStencilState, AClearDepthIsWhatApplesGpuStores)
{
    uint32_t bits;
    for (double nan : {std::nan(""), -std::nan("")})
        for (uint32_t format : {252u, 260u}) {
            const float d = gm20b::metalClearDepth(nan, format);
            memcpy(&bits, &d, 4);
            EXPECT_EQ(bits, 0x7fc00000u) << format;
        }
    EXPECT_EQ(gm20b::metalClearDepth(std::nan(""), 250), 0.0f);
    EXPECT_EQ(gm20b::metalClearDepth(1.0, 250), 1.0f);
    EXPECT_EQ(gm20b::metalClearDepth(2.0, 252), 1.0f);
    EXPECT_EQ(gm20b::metalClearDepth(-1.0, 252), 0.0f);
    EXPECT_EQ(gm20b::metalClearDepth(INFINITY, 260), 1.0f);
    EXPECT_EQ(gm20b::metalClearDepth(-INFINITY, 260), 0.0f);
    EXPECT_EQ(gm20b::metalClearDepth(0.25, 252), 0.25f);
}

// A Depth16Unorm clear stores Metal's code floor(float(d) x 65535) (0.25: 0x3fff) whether the chip rounds the float it is
// handed to the nearest code or truncates it, its product exact or rounded to a float: every code's own value and values
// a tenth, a half and nine tenths of a code above it
TEST(DepthStencilState, ADepth16ClearIsMetalsCodeUnderEitherRounding)
{
    const auto metal = [](double d) {
        return int64_t(std::floor(double(float(std::clamp(d, 0.0, 1.0))) * 65535.0));
    };
    const auto chips = [](float f) {
        const double exact = double(f) * 65535.0;
        const float rounded = f * 65535.0f;
        return std::array<int64_t, 4>{int64_t(std::nearbyint(exact)), int64_t(std::floor(exact)),
                                      int64_t(std::nearbyint(rounded)), int64_t(std::floor(rounded))};
    };
    EXPECT_EQ(metal(0.25), 0x3fff);
    uint32_t wrong = 0;
    for (int64_t c = 0; c <= 65535; c++)
        for (double frac : {0.0, 0.1, 0.5, 0.9}) {
            const double d = (double(c) + frac) / 65535.0;
            for (const int64_t stored : chips(gm20b::metalClearDepth(d, 250)))
                wrong += stored != metal(d);
        }
    EXPECT_EQ(wrong, 0u);
}

// A Depth16Unorm beside a Stencil8 is drawn through a Depth32Float_Stencil8 (TegraGM20BMetalRangeForms.mm depth16Zeta): each code
// in as the float the texture unit reads of it, c / 65535 rounded to a float, and back as a fragment depth the 16-bit zeta
// rounds to the nearest code (D3D11's FLOAT -> UNORM rule, which NVIDIA's ROP follows): every code returns as itself. A
// zeta that truncated would lose some, which HW Test gpu.gm20b.separate_depth16_stencil reads on the chip.
TEST(DepthStencilState, ADepth16CodeRoundTripsThroughAFloatDepth)
{
    uint32_t lostRounding = 0, lostTruncating = 0;
    for (uint32_t c = 0; c <= 65535; c++) {
        const float read = float(double(c) / 65535.0);
        lostRounding += uint32_t(std::nearbyint(double(read) * 65535.0)) != c;
        lostTruncating += uint32_t(std::floor(double(read) * 65535.0)) != c;
    }
    EXPECT_EQ(lostRounding, 0u);
    EXPECT_GT(lostTruncating, 0u) << "a truncating zeta would keep every code too: the HW Test reads which";
}

}  // namespace
