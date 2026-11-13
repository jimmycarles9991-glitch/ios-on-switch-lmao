// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../raw/runtime/raw_depth_stencil_state.h"

namespace {
using raw::DepthStencilDesc;
using raw::DepthStencilRegs;
using raw::DepthStencilState;
using raw::StencilFace;

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
                                     raw::depthStencilRegs(d, front, back, depth, stencil)))
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
                     raw::depthStencilRegs(DepthStencilDesc(), 3, 4, true, true)));
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
                    uint32_t words[raw::kDepthStencilBlockWords + raw::kStencilReferenceWords];
                    memcpy(words, state.block(depth, stencil), sizeof(raw::DepthStencilBlock));
                    raw::Push p{words + raw::kDepthStencilBlockWords, words + sizeof words / 4};
                    raw::stencilReferenceWords(p, front, back);
                    ASSERT_EQ(p.at, p.end);
                    DepthStencilRegs got;
                    bool seen[raw::kDepthStencilRegs] = {};
                    for (uint32_t w : words) {
                        const Immediate m = decode(w);
                        const auto *at = std::find(std::begin(raw::kDepthStencilMethods),
                                                   std::end(raw::kDepthStencilMethods), m.method);
                        ASSERT_NE(at, std::end(raw::kDepthStencilMethods)) << std::hex << m.method;
                        const size_t i = size_t(at - std::begin(raw::kDepthStencilMethods));
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
                    uint32_t words[raw::kDepthStencilRegs];
                    ASSERT_EQ(raw::depthStencilStateWords(want, words), raw::kDepthStencilRegs - 2);
                    DepthStencilRegs got;
                    bool seen[raw::kDepthStencilRegs] = {};
                    for (const uint32_t reference : {raw::kFrontStencilReference, raw::kBackStencilReference}) {
                        got.v[reference] = want.v[reference];
                        seen[reference] = true;
                    }
                    for (uint32_t w = 0; w < raw::kDepthStencilRegs - 2; w++) {
                        const Immediate m = decode(words[w]);
                        const auto *at = std::find(std::begin(raw::kDepthStencilMethods),
                                                   std::end(raw::kDepthStencilMethods), m.method);
                        ASSERT_NE(at, std::end(raw::kDepthStencilMethods)) << std::hex << m.method;
                        const size_t i = size_t(at - std::begin(raw::kDepthStencilMethods));
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
    uint32_t words[raw::kDepthStencilRegs];
    const uint32_t n = raw::depthStencilStateWords(state.regs(true, true, 0x5a, 0xa5), words);
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
            EXPECT_EQ(raw::testsHiddenZeta(d), f != 7) << int(f) << " write " << write;
        }
    DepthStencilDesc stencil;
    stencil.back.compare = 2;
    EXPECT_TRUE(raw::testsHiddenZeta(stencil));
    DepthStencilDesc replaces;
    replaces.front.pass = 2;
    EXPECT_TRUE(raw::testsHiddenZeta(replaces));
    DepthStencilDesc failOnly;
    failOnly.front.fail = 2;  // a compare of Always never fails
    EXPECT_FALSE(raw::testsHiddenZeta(failOnly));
}

}  // namespace
