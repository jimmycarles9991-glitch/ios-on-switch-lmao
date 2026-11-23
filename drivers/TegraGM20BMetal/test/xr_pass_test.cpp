// SPDX-License-Identifier: MIT
// Extended-range targets and two-plane reads: emit blends an extended-range target's RGBA16Float stand-in with Metal's
// factors; and nvk_switch_xr_planes.c's join of a header pair, its f32 steps as emitted, gives each code's value as the half
// Metal's read returns, for every code read as c / 1023 within an ulp and every alpha byte read as b / 255, and every step
// of Metal's filter grid its half.
#include "../emit.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The fragment header's output map (NAK sph.rs omap_target, from bit 576): `bits` of target j's components at 4j
std::array<uint8_t, 80> headerWriting(uint32_t bits)
{
    std::array<uint8_t, 80> h{};
    memcpy(h.data() + 72, &bits, 4);
    return h;
}

uint32_t registerOf(const std::vector<gm20b::EmitRegister> &regs, uint32_t method)
{
    for (const gm20b::EmitRegister &r : regs)
        if (r.method == method)
            return r.value;
    return 0xffffffffu;
}

// An extended-range target renders into its RGBA16Float stand-in, which the ROP blends: Metal's write mask and blend, a
// format of no alpha's factors of the destination alpha the constants they come to (BGR10_XR: DestinationAlpha one,
// OneMinusDestinationAlpha zero, SourceAlphaSaturated zero for colour and one for alpha)
TEST(XrPass, AnExtendedRangeTargetBlendsInItsStandIn)
{
    constexpr uint32_t kCtWrite = 0x1a00, kBlend = 0x1360, kPerTarget = 0x1e00;
    constexpr uint32_t kOne = 0x4001, kZero = 0x4000, kDstAlpha = 0x4304, kOneMinusDstAlpha = 0x4305;
    const std::array<uint8_t, 80> vs{}, fs = headerWriting(0xf);
    gm20b::EmitProgram vertex, fragment;
    vertex.header = vs.data();
    fragment.header = fs.data();
    std::vector<gm20b::EmitRegister> regs;
    std::string why;
    ASSERT_TRUE(gm20b::emitState("samples=1 topo=0 ds=0/0 c0=554:15:1:8:9:0:10:8:0", vertex, fragment, regs, why))
        << why;
    EXPECT_EQ(registerOf(regs, kCtWrite), 0x1111u) << "Metal's write mask, every component of the stand-in";
    EXPECT_EQ(registerOf(regs, kBlend), 1u);
    EXPECT_EQ(registerOf(regs, kPerTarget + 8), kOne) << "source colour: DestinationAlpha of a destination alpha of 1";
    EXPECT_EQ(registerOf(regs, kPerTarget + 12), kZero) << "destination colour: OneMinusDestinationAlpha";
    EXPECT_EQ(registerOf(regs, kPerTarget + 20), kOne) << "source alpha: SourceAlphaSaturated";
    EXPECT_EQ(registerOf(regs, kPerTarget + 24), kOne) << "destination alpha: DestinationAlpha";
    ASSERT_TRUE(gm20b::emitState("samples=1 topo=0 ds=0/0 c0=552:15:1:8:9:0:10:8:0", vertex, fragment, regs, why))
        << why;
    EXPECT_EQ(registerOf(regs, kBlend), 1u);
    EXPECT_EQ(registerOf(regs, kPerTarget + 8), kDstAlpha) << "BGRA10_XR holds its alpha";
    EXPECT_EQ(registerOf(regs, kPerTarget + 12), kOneMinusDstAlpha);
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
