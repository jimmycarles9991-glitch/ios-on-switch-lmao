// SPDX-License-Identifier: MIT
// raw/runtime/raw_zeta_colour.h on the host, the one-draw stencil route beside raw_stencil_bits.h's eight draws:
//  - its colour target is the zeta's memory: the SET_COLOR_TARGET_* words address, size and lay out what SET_ZT_* do for the
//    same zeta (zetaTargetValues), in RU32_GU32 or RU8, and its pass binds that target loaded and no zeta;
//  - which zetas it takes, and its states: the pack's lines make each with the write mask its draw needs;
//  - on a model of the ROP over ZF32_X24S8 texels (word 0 the depth's bits, word 1's low byte the stencil) and S8 texels,
//    its draw, through the mask the pack's line gives it, writes the stencil the eight draws write, a turn copy the depth
//    with it, a resolve keeps the depth word, and a texel outside a turn copy's source rectangle is kept.
#include <gtest/gtest.h>

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_target.h"
#include "../raw/runtime/raw_zeta_colour.h"
#include "bazel/testing/source_text.h"

namespace {

using raw::StencilSample;
using raw::TurnMap;
using switch_ios::test::SourceText;

struct FakeTexture {
    uint64_t va = 0;
    uint32_t width = 0, height = 0, format = 0, pitch = 0;
    uint8_t bpp = 0, blockHeight = 0, samples = 1;
    uint64_t layerBytes = 0;
    bool blockLinear = false, zeta = false, renderable = false, comptags = false;
    uint32_t tic = 0;
};

struct FakeAttachment {
    FakeTexture *texture = nullptr;

    enum Load : uint8_t {
        DontCare,
        Load,
        Clear
    } load = DontCare;

    bool store = false;
};

struct FakePass {
    FakeAttachment color[8];
    uint32_t count = 0;

    struct {
        FakeTexture *texture = nullptr;
    } depthStencil;

    uint32_t width = 0, height = 0;
};

// A zeta as zetaShape lays one out: one level of fx::mipChainLayout at its texel size
FakeTexture zeta(uint32_t format, uint32_t w, uint32_t h)
{
    const uint32_t bpp = raw::kZetaFormatInfo[raw::zetaFormatIndex(format)].bpp;
    raw::fx::MipChain c;
    EXPECT_TRUE(raw::fx::mipChainLayout(w, h, 1, bpp, c));
    FakeTexture z;
    z.va = 0x3456780000ull;
    z.width = w;
    z.height = h;
    z.format = format;
    z.bpp = uint8_t(bpp);
    z.pitch = c.level[0].rowBytes;
    z.blockLinear = true;
    z.blockHeight = c.level[0].yLog2;
    z.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
    z.zeta = true;
    z.tic = 7;
    return z;
}

// SET_COLOR_TARGET_A .. LAYER as the raw path writes them for `t` of render format `rt`
std::vector<uint32_t> colourWords(const FakeTexture &t, uint32_t rt)
{
    const switch_gm20b_target k = raw::kitTarget(raw::colorTargetDesc(t, rt, t.height));
    std::vector<uint32_t> words(SWITCH_GM20B_COLOR_TARGET_WORDS);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_color_target(&p, 0, &k, false);
    return std::vector<uint32_t>(words.begin() + 1, words.begin() + 10);
}

uint32_t renderFormat(uint32_t colour)
{
    return colour == 103 ? NVB197_SET_COLOR_TARGET_FORMAT_V_RU32_GU32 : NVB197_SET_COLOR_TARGET_FORMAT_V_RU8;
}

// The function of pack state `state` ("raw.stencil_bits.<function>")
std::string function(const char *state)
{
    const std::string s = state;
    return s.substr(s.rfind('.') + 1);
}

// The write mask the pack's line for `function` gives its target, its format checked against `format`
uint32_t lineMask(const std::string &function, uint32_t format)
{
    static const std::string lines = SourceText("../raw/pack/probe_states.h");
    std::smatch m;
    const std::regex line("\"c0=(\\d+):(\\d+):0:1:0:0:1:0:0 \\| v stencil_bits_vert \\| f " + function + "\"");
    if (!std::regex_search(lines, m, line)) {
        ADD_FAILURE() << "no pack line for " << function;
        return 0;
    }
    EXPECT_EQ(std::stoul(m[1]), format) << function;
    return uint32_t(std::stoul(m[2]));
}

const std::pair<uint32_t, StencilSample> kForms[] = {
    {1, StencilSample::Zero},       {4, StencilSample::Zero},          {4, StencilSample::Last},
    {4, StencilSample::LeastDepth}, {4, StencilSample::GreatestDepth},
};

// The route's colour target is the zeta's memory as the zeta target words have it, in its words' format
TEST(RawZetaColour, TheColourTargetIsTheZetasMemory)
{
    const uint32_t sizes[][2] = {{1, 1}, {90, 60}, {96, 64}, {1280, 720}, {720, 1280}, {4096, 4096}};
    for (const uint32_t format : {260u, 253u})
        for (const auto &s : sizes) {
            const FakeTexture z = zeta(format, s[0], s[1]);
            const FakeTexture view = raw::zetaColourView(z);
            EXPECT_FALSE(view.zeta);
            EXPECT_TRUE(view.renderable);
            EXPECT_EQ(view.tic, 0u);
            EXPECT_EQ(view.format, format == 260 ? 103u : 13u);
            EXPECT_EQ(view.bpp, z.bpp);
            const raw::ZetaTargetValues zt = raw::zetaTargetValues(z, z.height);
            const std::vector<uint32_t> w = colourWords(view, renderFormat(view.format));
            EXPECT_EQ(w[0], zt.a[0]);
            EXPECT_EQ(w[1], zt.a[1]);
            EXPECT_EQ(w[2], zt.size[0]) << "WIDTH: the zeta's row stride in texels";
            EXPECT_EQ(w[3], zt.size[1]);
            EXPECT_EQ(w[4], renderFormat(view.format));
            EXPECT_EQ(w[5], zt.a[3]) << "MEMORY: one GOB wide, the zeta's block height, BLOCKLINEAR";
            EXPECT_EQ(w[6], 1u);
            EXPECT_EQ(w[7], zt.a[4]) << "ARRAY_PITCH: the zeta's";
            EXPECT_EQ(w[8], 0u);
        }
}

// The formats table renders RG32Uint as RU32_GU32 and R8Uint as RU8, and samples R8Uint as Stencil8's own header does
TEST(RawZetaColour, TheFormatsTableRendersTheViews)
{
    const std::string formats = SourceText("../raw/runtime/raw_gpu.cpp");
    std::smatch m;
    ASSERT_TRUE(std::regex_search(formats, m, std::regex("\\{103, 0x([0-9a-f]+), 0x([0-9a-f]+), 8,")));
    EXPECT_EQ(std::stoul(m[1], nullptr, 16), uint32_t(NVB197_SET_COLOR_TARGET_FORMAT_V_RU32_GU32));
    ASSERT_TRUE(std::regex_search(formats, m, std::regex("\\{13, 0x([0-9a-f]+), 0x([0-9a-f]+), 1,")));
    EXPECT_EQ(std::stoul(m[1], nullptr, 16), uint32_t(NVB197_SET_COLOR_TARGET_FORMAT_V_RU8));
    EXPECT_EQ(std::stoul(m[2], nullptr, 16), raw::kZetaFormatInfo[raw::zetaFormatIndex(253)].tic0);
}

// One colour target, loaded and stored, over the area asked; no zeta bound
TEST(RawZetaColour, ThePassBindsTheViewAlone)
{
    FakeTexture view = raw::zetaColourView(zeta(260, 96, 64));
    const FakePass p = raw::zetaColourPass<FakePass>(view, 90, 60);
    EXPECT_EQ(p.color[0].texture, &view);
    EXPECT_EQ(p.color[0].load, FakeAttachment::Load);
    EXPECT_TRUE(p.color[0].store);
    EXPECT_EQ(p.count, 1u);
    EXPECT_EQ(p.width, 90u);
    EXPECT_EQ(p.height, 60u);
    EXPECT_EQ(p.depthStencil.texture, nullptr);
}

// Taken while the fact or the probe holds, into a one-sample zeta with stencil and without comptags
TEST(RawZetaColour, WhichZetasItWrites)
{
    FakeTexture d32s8 = zeta(260, 8, 8), s8 = zeta(253, 8, 8);
    EXPECT_TRUE(raw::zetaColourWrites(d32s8, true));
    EXPECT_TRUE(raw::zetaColourWrites(s8, true));
    EXPECT_FALSE(raw::zetaColourWrites(d32s8, false));
    FakeTexture compressed = d32s8, multisampled = d32s8;
    compressed.comptags = true;
    multisampled.samples = 4;
    EXPECT_FALSE(raw::zetaColourWrites(compressed, true));
    EXPECT_FALSE(raw::zetaColourWrites(multisampled, true));
    for (const uint32_t depthOnly : {250u, 252u})
        EXPECT_FALSE(raw::zetaColourWrites(zeta(depthOnly, 8, 8), true)) << depthOnly;
}

// A state for each form the eight draws have, none other; the pack's line makes each: from one sample the whole texel, from
// several Depth32Float_Stencil8's G alone (MTLColorWriteMaskGreen), so the resolved depth stays
TEST(RawZetaColour, ThePackMakesEachState)
{
    const std::string metal = SourceText("../raw/pack/stencil_bits.metal");
    const std::string keys = SourceText("../raw/runtime/raw_record_parts.cpp");
    for (const uint32_t format : {253u, 260u, 252u})
        for (const auto &[samples, pick] : kForms) {
            const char *colour = raw::stencilColourState(format, samples, pick);
            EXPECT_EQ(colour == nullptr, raw::stencilBitsState(format, samples, pick) == nullptr)
                << format << " " << samples << " " << int(pick);
            if (!colour)
                continue;
            const std::string f = function(colour);
            EXPECT_EQ(std::string(colour), "raw.stencil_bits." + f);
            EXPECT_NE(metal.find("fragment uint4 " + f + "("), std::string::npos) << f;
            EXPECT_NE(keys.find("\"stencil_bits_vert | f " + f + "\""), std::string::npos) << f;
            EXPECT_EQ(lineMask(f, raw::zetaColourFormat(format)), format == 260 && samples > 1 ? 4u : 15u) << f;
        }
}

// ZF32_X24S8 texels as the colour ROP and the zeta see them, or S8's (word 0 the stencil)
struct Texel {
    uint32_t word[2] = {};

    uint8_t stencil(uint32_t format) const
    {
        return uint8_t(word[format == 260 ? 1 : 0]);
    }
};

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<Texel> texels;

    Texel &at(int32_t x, int32_t y)
    {
        return texels[size_t(y) * width + size_t(x)];
    }
};

bool inside(const TurnMap &m, int32_t x, int32_t y)
{
    return x >= m.x0 && y >= m.y0 && x < m.x1 && y < m.y1;
}

// The route's draw into `target` from `source` at `map`: the program's output (pack/stencil_bits.metal *_colour_frag: a
// whole texel from one sample, the stencil alone into G or R from a resolve) through the write mask its pack line gives
void drawColour(Image &target, Image &source, const TurnMap &map, uint32_t format, uint32_t samples)
{
    const uint32_t mask = lineMask(function(raw::stencilColourState(format, samples, StencilSample::Zero)),
                                   raw::zetaColourFormat(format));
    const raw::StencilBitsArgs a = raw::stencilBitsArgs(map, samples, StencilSample::Zero);
    for (int32_t y = 0; y < int32_t(target.height); y++)
        for (int32_t x = 0; x < int32_t(target.width); x++) {
            const int32_t sx = a.map.ax * x + a.map.ay * y + a.map.cx, sy = a.map.bx * x + a.map.by * y + a.map.cy;
            if (!inside(a.map, sx, sy))
                continue;
            const Texel &s = source.at(sx, sy);
            const uint32_t out[2] = {format == 253 ? s.stencil(format)
                                     : samples > 1 ? 0u
                                                   : s.word[0],
                                     format == 260 ? s.stencil(format) : 0u};
            for (uint32_t c = 0; c < 2; c++)
                if (mask & (8u >> c))
                    target.at(x, y).word[c] = out[c];
        }
}

// raw_stencil_bits.h's draws into `target`'s stencil (zeroed over the source rectangle first) and the depth copy beside them
// for one sample
void drawBits(Image &target, Image &source, const TurnMap &map, uint32_t format, uint32_t samples)
{
    const auto texels = [&](auto write) {
        for (int32_t y = 0; y < int32_t(target.height); y++)
            for (int32_t x = 0; x < int32_t(target.width); x++) {
                const int32_t sx = map.ax * x + map.ay * y + map.cx, sy = map.bx * x + map.by * y + map.cy;
                if (inside(map, sx, sy))
                    write(target.at(x, y), source.at(sx, sy));
            }
    };
    const uint32_t word = format == 260 ? 1 : 0;
    texels([&](Texel &t, const Texel &s) {
        t.word[word] &= ~0xffu;
        if (format == 260 && samples == 1)
            t.word[0] = s.word[0];
    });
    raw::forEachStencilBit(
        raw::stencilBitsArgs(map, samples, StencilSample::Zero), [&](uint32_t k, const raw::StencilBitsArgs &a) {
            const raw::DepthStencilRegs r = raw::depthStencilRegs(raw::stencilBitState(k), raw::kStencilBitReference,
                                                                  raw::kStencilBitReference, format == 260, true);
            const uint32_t ref = r.v[raw::kFrontStencilReference], writeMask = r.v[raw::kFrontStencilReference + 2];
            texels([&](Texel &t, const Texel &s) {
                if (s.stencil(format) & a.bit)
                    t.word[word] = (t.word[word] & ~writeMask) | (ref & writeMask);
            });
            return true;
        });
}

Image filled(uint32_t w, uint32_t h, uint32_t seed)
{
    Image im{w, h, {}};
    for (uint32_t i = 0; i < w * h; i++)
        im.texels.push_back({{0x3f000000u + i * 2654435761u + seed, (i * 37 + seed) & 0xff}});
    return im;
}

// A turn copy moves every texel's depth and stencil where Metal stores it and back, as the eight draws and the depth copy
// do, and keeps the texels outside its source rectangle
TEST(RawZetaColour, ATurnCopyWritesWhatTheEightDrawsWrite)
{
    for (const uint32_t format : {260u, 253u})
        for (uint8_t turn = 1; turn <= 4; turn++) {
            raw::Rotation r;
            r.turn = turn;
            r.width = 96;
            r.height = 64;
            Image frame = filled(r.width, r.height, 5), plane = filled(r.planeWidth(), r.planeHeight(), 9);
            Image bitsPlane = plane;
            drawColour(plane, frame, raw::frameToPlane(r), format, 1);
            drawBits(bitsPlane, frame, raw::frameToPlane(r), format, 1);
            for (uint32_t i = 0; i < plane.texels.size(); i++) {
                ASSERT_EQ(plane.texels[i].stencil(format), bitsPlane.texels[i].stencil(format))
                    << int(turn) << " " << i;
                if (format == 260)
                    ASSERT_EQ(plane.texels[i].word[0], bitsPlane.texels[i].word[0]) << int(turn) << " " << i;
            }
            Image back = filled(r.width, r.height, 3);
            const TurnMap area = raw::within(raw::planeToFrame(r), {0, 0, r.planeWidth() / 2, r.planeHeight()});
            const Image before = back;
            drawColour(back, plane, area, format, 1);
            uint32_t kept = 0;
            for (int32_t y = 0; y < int32_t(r.height); y++)
                for (int32_t x = 0; x < int32_t(r.width); x++) {
                    const int32_t sx = area.ax * x + area.ay * y + area.cx, sy = area.bx * x + area.by * y + area.cy;
                    const Texel &want = inside(area, sx, sy) ? frame.at(x, y) : before.texels[size_t(y) * r.width + x];
                    kept += !inside(area, sx, sy);
                    ASSERT_EQ(back.at(x, y).stencil(format), want.stencil(format)) << int(turn) << " " << x << "," << y;
                    if (format == 260)
                        ASSERT_EQ(back.at(x, y).word[0], want.word[0]) << int(turn) << " " << x << "," << y;
                }
            EXPECT_GT(kept, 0u);
        }
}

// A resolve writes the stencil the eight draws write and keeps the depth word its depth resolve wrote
TEST(RawZetaColour, AResolveKeepsTheDepthWord)
{
    for (const uint32_t format : {260u, 253u}) {
        Image source = filled(90, 60, 11), colour = filled(90, 60, 4);
        Image bits = colour;
        drawColour(colour, source, TurnMap{}, format, 4);
        drawBits(bits, source, TurnMap{}, format, 4);
        for (uint32_t i = 0; i < colour.texels.size(); i++) {
            EXPECT_EQ(colour.texels[i].stencil(format), source.texels[i].stencil(format)) << i;
            EXPECT_EQ(colour.texels[i].stencil(format), bits.texels[i].stencil(format)) << i;
            if (format == 260)
                EXPECT_EQ(colour.texels[i].word[0], bits.texels[i].word[0]) << "the depth word kept " << i;
        }
    }
}

}  // namespace
