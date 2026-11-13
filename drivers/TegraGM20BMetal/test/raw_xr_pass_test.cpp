// SPDX-License-Identifier: MIT
// raw_xr_pass.h: RGB10A8_2P_XR10 drawn in its two planes. The alpha index is m2v's (pipeline/two_plane.rs: the lowest free
// index, in index order); a state's colour formats and a Clear's plane words are the planes'; raw_emit writes each alpha
// plane's target; and nvk_switch_xr_planes.c's join of a header pair, its f32 steps as emitted, gives each code's value as
// the half Metal's read returns, for every code read as c / 1023 within an ulp and every alpha byte read as b / 255, and
// every step of Metal's filter grid its half.
#include "../raw/runtime/raw_emit.h"
#include "../raw/runtime/raw_xr_pass.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace {

using Slots = std::array<int8_t, 8>;

Slots slots(std::initializer_list<int> v)
{
    Slots out;
    out.fill(-1);
    int i = 0;
    for (int s : v)
        out[i++] = int8_t(s);
    return out;
}

// The pack generator's copy of the rule (raw/pack/mkpack.py xr_alpha_slots) takes these cases too: mkpack_owned_test
TEST(XrPass, TheAlphaIndexIsTheLowestFreeOne)
{
    const uint32_t one[8] = {550};
    EXPECT_EQ(raw::xrAlphaSlots(one), slots({1}));
    const uint32_t two[8] = {550, 550};
    EXPECT_EQ(raw::xrAlphaSlots(two), slots({2, 3}));
    const uint32_t renderBox[8] = {550, 105, 550};
    EXPECT_EQ(raw::xrAlphaSlots(renderBox), slots({3, -1, 4}));
    const uint32_t after[8] = {80, 550};
    EXPECT_EQ(raw::xrAlphaSlots(after), slots({-1, 2}));
    const uint32_t hole[8] = {550, 0, 80};
    EXPECT_EQ(raw::xrAlphaSlots(hole), slots({1}));
    const uint32_t full[8] = {550, 80, 80, 80, 80, 80, 80, 80};
    EXPECT_EQ(raw::xrAlphaSlots(full), slots({}));
    const uint32_t other[8] = {552, 554};
    EXPECT_EQ(raw::xrAlphaSlots(other), slots({}));
}

TEST(XrPass, AStateTakesThePlanesFormats)
{
    uint32_t formats[8] = {550, 105, 550};
    raw::xrPlaneFormats(formats);
    const uint32_t want[8] = {53, 105, 53, 13, 13};
    EXPECT_EQ(memcmp(formats, want, sizeof want), 0);
}

TEST(XrPass, AClearsPlaneWordsAreItsTexels)
{
    const float colors[][4] = {{0.5f, -0.5f, 1.0f, 0.75f},
                               {1.125f, 0.125f, -0.625f, 0.25f},
                               {NAN, 2.0f, -2.0f, NAN},
                               {0, 0, 0, 0},
                               {-0.7529f, 1.2529f, 0.3f, 1.5f}};
    for (const auto &c : colors) {
        uint32_t texel[4];
        raw::xrClearTexel(raw::XrForm::Planes, c, false, texel);
        const std::array<uint32_t, 2> words = raw::xrPlaneClears(c);
        EXPECT_EQ(words[0], texel[0]);
        EXPECT_EQ(words[1], texel[1]);
        EXPECT_EQ(words[0] >> 30, 0u);
        EXPECT_LE(words[1], 255u);
    }
}

// The fragment header's output map (NAK sph.rs omap_target, from bit 576): `bits` of target j's components at 4j
std::array<uint8_t, 80> headerWriting(uint32_t bits)
{
    std::array<uint8_t, 80> h{};
    memcpy(h.data() + 72, &bits, 4);
    return h;
}

uint32_t registerOf(const std::vector<raw::EmitRegister> &regs, uint32_t method)
{
    for (const raw::EmitRegister &r : regs)
        if (r.method == method)
            return r.value;
    return 0xffffffffu;
}

TEST(XrPass, TheAlphaPlanesTargetIsWritten)
{
    constexpr uint32_t kCtWrite = 0x1a00, kBlend = 0x1360;
    const std::array<uint8_t, 80> vs{}, fs = headerWriting(1u | 1u << 4);
    raw::EmitProgram vertex, fragment;
    vertex.header = vs.data();
    fragment.header = fs.data();
    std::vector<raw::EmitRegister> regs;
    std::string why;
    ASSERT_TRUE(
        raw::emitState("samples=1 topo=0 ds=0/0 c0=550:15:1:1:5:0:1:5:0", vertex, fragment, regs, why, nullptr, true))
        << why;
    EXPECT_EQ(registerOf(regs, kCtWrite), 1u);
    EXPECT_EQ(registerOf(regs, kCtWrite + 4), 1u);
    EXPECT_EQ(registerOf(regs, kBlend), 0u);
    EXPECT_EQ(registerOf(regs, kBlend + 4), 0u);
    ASSERT_TRUE(
        raw::emitState("samples=1 topo=0 ds=0/0 c0=550:0:0:1:0:0:1:0:0", vertex, fragment, regs, why, nullptr, true))
        << why;
    EXPECT_EQ(registerOf(regs, kCtWrite + 4), 0u);
    ASSERT_TRUE(raw::emitState("samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0", vertex, fragment, regs, why)) << why;
    EXPECT_EQ(registerOf(regs, kCtWrite + 4), 0xffffffffu) << "the one-image form has no alpha target";
}

// nvk_switch_xr_planes.c decode: the read on Metal's grid of 16 steps a code, half up, less the zero code's steps, times
// the grid's reciprocal, each an f32 rounded on its own, then the half nearest
float joined(float v, double codes, double zero, double scale)
{
    volatile float scaled = v * float(16.0 * codes);
    volatile float step = std::floor(scaled + 0.5f);
    volatile float steps = step + float(-16.0 * zero);
    volatile float x = steps * float(1.0 / (16.0 * scale));
    return float(_Float16(x));
}

float halfOf(double exact)
{
    return float(_Float16(float(exact)));
}

TEST(XrPass, TheJoinGivesEachCodesHalf)
{
    for (uint32_t code = 0; code < 1024; code++) {
        const float read = float(code) / 1023.0f;
        const float want = halfOf((double(code) - 384.0) / 510.0);
        for (float v : {read, std::nextafter(read, 0.0f), std::nextafter(read, 2.0f)})
            EXPECT_EQ(joined(v, 1023, 384, 510), want) << "code " << code << " read " << v;
    }
    for (uint32_t byte = 0; byte < 256; byte++) {
        const float read = float(byte) / 255.0f;
        const float want = halfOf(double(byte) / 255.0);
        for (float v : {read, std::nextafter(read, 0.0f), std::nextafter(read, 2.0f)})
            EXPECT_EQ(joined(v, 255, 0, 255), want) << "byte " << byte;
    }
}

// Every step of a filtered read's grid (a sixteenth of a code) gives the half nearest its value, as Metal's linear filter
// rounds its step (m2v lower/sampler/extended.rs)
TEST(XrPass, EveryGridStepGivesItsHalf)
{
    for (int32_t step = 0; step <= 16 * 1023; step++) {
        const float read = float(double(step) / (16.0 * 1023.0));
        EXPECT_EQ(joined(read, 1023, 384, 510), halfOf((double(step) / 16.0 - 384.0) / 510.0)) << "step " << step;
    }
    for (int32_t step = 0; step <= 16 * 255; step++) {
        const float read = float(double(step) / (16.0 * 255.0));
        EXPECT_EQ(joined(read, 255, 0, 255), halfOf(double(step) / (16.0 * 255.0))) << "alpha step " << step;
    }
}

}  // namespace
