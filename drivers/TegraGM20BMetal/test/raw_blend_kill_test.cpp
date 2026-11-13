// SPDX-License-Identifier: MIT
// raw/runtime/raw_blend_kill.h on the host: the zero-source pixel kill is on exactly for the single-target ROP blends
// under which a source of (0, 0, 0, 0) leaves every destination value as it was, and for none other.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "../raw/runtime/raw_blend_kill.h"

namespace {

enum : uint32_t {
    kAdd = 0x8006,
    kSub = 0x800a,
    kRevSub = 0x800b,
    kMin = 0x8007,
    kMax = 0x8008,
    kZero = 0x4000,
    kOne = 0x4001,
    kSrcColor = 0x4300,
    kInvSrcColor = 0x4301,
    kSrcAlpha = 0x4302,
    kInvSrcAlpha = 0x4303,
    kDstAlpha = 0x4304,
    kInvDstAlpha = 0x4305,
    kDstColor = 0x4306,
    kInvDstColor = 0x4307,
    kSaturate = 0x4308,
    kConstColor = 0xc001,
    kSrc1Color = 0xc900,
    kInvSrc1Alpha = 0xc903,
};

struct Blend {
    uint32_t op, src, dst;
};

struct State {
    Blend rgb{kAdd, kOne, kInvSrcAlpha}, alpha{kAdd, kOne, kInvSrcAlpha};
    bool blending = true;
    uint32_t format = 115;  // RGBA16Float
    std::string ds = "0/0";
    uint32_t alphaControl = 0;
    uint32_t writes[8] = {0x1111};
    uint8_t storage = 0;
};

uint32_t decide(const State &s)
{
    const std::vector<uint32_t> blob = {
        0x153c, s.alphaControl, 0x1360, s.blending ? 1u : 0u, 0x1e04, s.rgb.op,    0x1e08, s.rgb.src,
        0x1e0c, s.rgb.dst,      0x1e10, s.alpha.op,           0x1e14, s.alpha.src, 0x1e18, s.alpha.dst,
    };
    const std::string key =
        "samples=1 topo=3 ds=" + s.ds + " c0=" + std::to_string(s.format) + ":15:1:1:5:0:1:5:0 | v vs | f fs";
    return raw::iteratedBlendNoop(blob.data(), uint32_t(blob.size()), s.writes, key.c_str(), s.storage);
}

// What the state's ROP writes for a zero source over destination `d` (a target's value as a float)
float blended(const Blend &b, float d, float srcAlphaZeroDstAlpha)
{
    const auto coeff = [&](uint32_t c) -> float {
        switch (c) {
        case kZero:
        case kSrcColor:
        case kSrcAlpha:
            return 0;
        case kOne:
        case kInvSrcColor:
        case kInvSrcAlpha:
            return 1;
        case kDstAlpha:
            return srcAlphaZeroDstAlpha;
        case kInvDstAlpha:
            return 1 - srcAlphaZeroDstAlpha;
        case kDstColor:
            return d;
        case kInvDstColor:
            return 1 - d;
        case kSaturate:
            return std::fmin(0.0f, 1 - srcAlphaZeroDstAlpha);
        default:
            return std::numeric_limits<float>::quiet_NaN();
        }
    };
    const float s = 0 * coeff(b.src), t = d * coeff(b.dst);
    switch (b.op) {
    case kAdd:
        return s + t;
    case kSub:
        return s - t;
    case kRevSub:
        return t - s;
    case kMin:
        return std::fmin(0.0f, d);
    default:
        return std::fmax(0.0f, d);
    }
}

TEST(BlendKill, PremultipliedAndStraightSourceOverKill)
{
    EXPECT_EQ(decide(State{}), raw::kNoopSourceRgba0000);
    State straight;
    straight.rgb = straight.alpha = {kAdd, kSrcAlpha, kInvSrcAlpha};
    EXPECT_EQ(decide(straight), raw::kNoopSourceRgba0000);
    State additive;
    additive.rgb = additive.alpha = {kAdd, kOne, kOne};
    EXPECT_EQ(decide(additive), raw::kNoopSourceRgba0000);
    State screen;
    screen.rgb = {kAdd, kOne, kInvSrcColor};
    EXPECT_EQ(decide(screen), raw::kNoopSourceRgba0000);
    State reverse;
    reverse.rgb = reverse.alpha = {kRevSub, kOne, kOne};
    EXPECT_EQ(decide(reverse), raw::kNoopSourceRgba0000);
}

// Every op and coefficient pair the rule takes leaves every destination value of its target kind as it was
TEST(BlendKill, KilledExactlyWhereTheBlendKeepsTheDestination)
{
    const uint32_t ops[] = {kAdd, kSub, kRevSub, kMin, kMax};
    const uint32_t coeffs[] = {kZero,        kOne,        kSrcColor,    kInvSrcColor, kSrcAlpha,
                               kInvSrcAlpha, kDstAlpha,   kInvDstAlpha, kDstColor,    kInvDstColor,
                               kSaturate,    kConstColor, kSrc1Color,   kInvSrc1Alpha};
    const float unormValues[] = {0.0f, 1.0f, 0.25f, 0.5f, 1.0f / 255};
    const float floatValues[] = {0.0f,
                                 -0.0f,
                                 1.0f,
                                 -3.5f,
                                 65504.0f,
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(),
                                 6e-8f};
    for (const bool normalized : {true, false})
        for (uint32_t op : ops)
            for (uint32_t src : coeffs)
                for (uint32_t dst : coeffs) {
                    State s;
                    s.format = normalized ? 80 : 115;
                    s.rgb = s.alpha = {op, src, dst};
                    const bool killed = decide(s) == raw::kNoopSourceRgba0000;
                    bool keeps = true;
                    for (float d : normalized ? std::vector<float>(std::begin(unormValues), std::end(unormValues))
                                              : std::vector<float>(std::begin(floatValues), std::end(floatValues)))
                        for (float da : normalized
                                            ? std::vector<float>(std::begin(unormValues), std::end(unormValues))
                                            : std::vector<float>(std::begin(floatValues), std::end(floatValues))) {
                            const float r = blended(s.rgb, d, da);
                            keeps &= r == d || (std::isnan(r) && std::isnan(d));
                        }
                    if (killed)
                        EXPECT_TRUE(keeps) << "op " << std::hex << op << " src " << src << " dst " << dst
                                           << (normalized ? " unorm" : " float");
                }
}

TEST(BlendKill, OffWhereTheSourceIsWrittenOrOtherOutputsMatter)
{
    State off;
    off.blending = false;
    EXPECT_EQ(decide(off), raw::kNoopNever);
    State subtract;
    subtract.rgb = {kSub, kOne, kOne};
    EXPECT_EQ(decide(subtract), raw::kNoopNever);
    State minimum;
    minimum.alpha = {kMin, kOne, kOne};
    EXPECT_EQ(decide(minimum), raw::kNoopNever);
    State constant;
    constant.rgb = {kAdd, kConstColor, kInvSrcAlpha};
    EXPECT_EQ(decide(constant), raw::kNoopNever);
    State dual;
    dual.rgb = {kAdd, kOne, kInvSrc1Alpha};
    EXPECT_EQ(decide(dual), raw::kNoopNever);
    State multiply;
    multiply.rgb = {kAdd, kDstColor, kZero};
    EXPECT_EQ(decide(multiply), raw::kNoopNever);
}

TEST(BlendKill, DestinationFactorsOnlyOnNormalizedTargets)
{
    State under;
    under.rgb = under.alpha = {kAdd, kInvDstAlpha, kOne};
    under.format = 80;  // BGRA8Unorm
    EXPECT_EQ(decide(under), raw::kNoopSourceRgba0000);
    under.format = 115;  // RGBA16Float: 0 times an infinite destination alpha is NaN
    EXPECT_EQ(decide(under), raw::kNoopNever);
    under.format = 81;  // BGRA8Unorm_sRGB
    EXPECT_EQ(decide(under), raw::kNoopSourceRgba0000);
}

TEST(BlendKill, OffForEveryOtherShapeOfState)
{
    State depth;
    depth.ds = "252/0";
    EXPECT_EQ(decide(depth), raw::kNoopNever);
    State coverage;
    coverage.alphaControl = 1;
    EXPECT_EQ(decide(coverage), raw::kNoopNever);
    State toOne;
    toOne.alphaControl = 0x10;
    EXPECT_EQ(decide(toOne), raw::kNoopNever);
    State two;
    two.writes[1] = 0x1111;
    EXPECT_EQ(decide(two), raw::kNoopNever);
    State unwritten;
    unwritten.writes[0] = 0;
    EXPECT_EQ(decide(unwritten), raw::kNoopNever);
    State storage;
    storage.storage = 1;
    EXPECT_EQ(decide(storage), raw::kNoopNever);
    State extended;
    extended.format = 552;  // BGRA10_XR
    EXPECT_EQ(decide(extended), raw::kNoopNever);
    State partialMask;
    partialMask.writes[0] = 0x0111;
    EXPECT_EQ(decide(partialMask), raw::kNoopSourceRgba0000);
}

TEST(BlendKill, StateWithoutPerTargetRegistersStaysOff)
{
    const std::vector<uint32_t> blob = {0x1360, 1};
    const uint32_t writes[8] = {0x1111};
    EXPECT_EQ(raw::iteratedBlendNoop(blob.data(), uint32_t(blob.size()), writes,
                                     "samples=1 topo=3 ds=0/0 c0=80:15:1:1:5:0:1:5:0", 0),
              raw::kNoopNever);
}

}  // namespace
