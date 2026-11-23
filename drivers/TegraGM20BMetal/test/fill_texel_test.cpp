// SPDX-License-Identifier: MIT
// The texels fillTexture:...color: writes (fill_texel.h) against what an Apple M4 under macOS 27.0.1 wrote (m2v's
// probes fil.m: EVs FRT-fil1-m4 and FRT-fil2-m4, m2v tests/fill_value.rs), and the rules' edges a Clear would store
// otherwise.
#include "../fill_texel.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using Format = uint32_t;

std::string texel(Format format, double r, double g, double b, double a)
{
    const double rgba[4] = {r, g, b, a};
    uint8_t out[16];
    const uint32_t n = gm20b::fillTexel(format, rgba, out);
    std::string s;
    char byte[3];
    for (uint32_t i = 0; i < n; ++i) {
        snprintf(byte, sizeof byte, "%02x", out[i]);
        s += byte;
    }
    return s;
}

uint64_t code(Format format, double x)
{
    const double rgba[4] = {x, 0, 0, 0};
    uint8_t out[16];
    const uint32_t n = gm20b::fillTexel(format, rgba, out);
    uint64_t v = 0;
    for (uint32_t i = n; i-- > 0;)
        v = v << 8 | out[i];
    return v;
}

double bits(uint64_t b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

// Three colours packed in each format as the M4 wrote them (EV FRT-fil1-m4)
TEST(FillTexel, MetalsMeasuredTexels)
{
    struct Case {
        Format format;
        const char *a, *b, *c;
    };

    const double colours[3][4] = {{0.25, 0.5, 0.75, 1.0}, {2.0, -1.0, 0.5, 0.3}, {0, 0, 0, 0}};
    for (const Case &k : {Case{10, "40", "ff", "00"},
                          Case{12, "20", "7f", "00"},
                          Case{13, "00", "02", "00"},
                          Case{20, "0040", "ffff", "0000"},
                          Case{25, "0034", "0040", "0000"},
                          Case{30, "4080", "ff00", "0000"},
                          Case{40, "1744", "10f8", "0000"},
                          Case{41, "2f44", "20f8", "0000"},
                          Case{42, "bf48", "85f0", "0000"},
                          Case{43, "17a2", "107c", "0000"},
                          Case{55, "0000803e", "00000040", "00000000"},
                          Case{65, "00340038", "004000bc", "00000000"},
                          Case{70, "4080bfff", "ff00804d", "00000000"},
                          Case{71, "89bce1ff", "ff00bc4d", "00000000"},
                          Case{80, "bf8040ff", "8000ff4d", "00000000"},
                          Case{81, "e1bc89ff", "bc00ff4d", "00000000"},
                          Case{90, "0001f8ef", "ff030060", "00000000"},
                          Case{92, "40031c74", "00040070", "00000000"},
                          Case{93, "8000027e", "00010089", "00000000"},
                          Case{94, "ff0208d0", "0002f07f", "00000000"},
                          Case{110, "00400080ffbfffff", "ffff00000080cd4c", "0000000000000000"},
                          Case{115, "00340038003a003c", "004000bc0038cd34", "0000000000000000"},
                          Case{125, "0000803e0000003f0000403f0000803f", "00000040000080bf0000003f9a99993e",
                               "00000000000000000000000000000000"}}) {
        SCOPED_TRACE(k.format);
        const char *want[3] = {k.a, k.b, k.c};
        for (int i = 0; i < 3; ++i)
            EXPECT_EQ(texel(k.format, colours[i][0], colours[i][1], colours[i][2], colours[i][3]), want[i]) << i;
    }
}

// Unsigned normalized: a 4- or 8-bit channel rounds the exact product half up, any other width the product's float; a
// Clear stores 0 at each of these
TEST(FillTexel, UnormRoundsItsProductByWidth)
{
    EXPECT_EQ(code(20, 0.5 / 65535.0), 1u);
    EXPECT_EQ(texel(90, 0.5 / 1023.0, 0, 0, 0), "01000000");
    EXPECT_EQ(texel(41, 0, 0, 0, 0.5), "0100");
    EXPECT_EQ(code(10, 0.25), 64u);
    EXPECT_EQ(code(10, NAN), 0u);
}

// sRGB: the code whose decoded value is nearest (a Clear's encoding curve stores 0xf9 for the first)
TEST(FillTexel, SrgbTakesTheNearestDecodedCode)
{
    EXPECT_EQ(code(11, 0.9429933939111084), 0xf8u);
    EXPECT_EQ(code(11, 0.9004610341771649), 0xf3u);
    EXPECT_EQ(code(11, 0.5), 0xbcu);
    EXPECT_EQ(code(11, -1.0), 0u);
    EXPECT_EQ(code(11, 2.0), 0xffu);
}

// Integers truncate and saturate the double; a quiet NaN is 0 (the smallest signed), a signalling one the largest
TEST(FillTexel, IntegersTruncateAndSortNaNsByKind)
{
    const double quiet = bits(0x7ff8000000000000), signalling = bits(0xfff0000000000001);
    EXPECT_EQ(code(13, quiet), 0u);
    EXPECT_EQ(code(13, signalling), 0xffu);
    EXPECT_EQ(code(14, quiet), 0x80u);
    EXPECT_EQ(code(14, signalling), 0x7fu);
    EXPECT_EQ(code(53, 2147483647.25), 0x80000000u);
    EXPECT_EQ(code(53, 2147483647.0), 0x7fffffffu);
    EXPECT_EQ(code(54, -2147483648.5), 0x80000000u);
    EXPECT_EQ(code(24, -2.5), 0xfffeu);
}

// Floats keep a NaN's sign and the top of its payload, quieted; RG11B10Float cuts below its smallest normal first
TEST(FillTexel, FloatsKeepNaNPayloadsAndSmallFloatsCutFirst)
{
    EXPECT_EQ(code(25, bits(0x7ff0040000000000)), 0x7e01u);
    EXPECT_EQ(code(25, bits(0xfff4000000000000)), 0xff00u);
    EXPECT_EQ(code(25, 65519.99999999999), 0x7c00u);
    EXPECT_EQ(code(55, bits(0x7ff0000020000000)), 0x7fc00001u);
    const auto red = [](double x) {
        return code(92, x) & 0x7ff;
    };
    EXPECT_EQ(red(std::ldexp(1.0, -21) * (1.0 + std::ldexp(1.0, -17))), 0u);
    EXPECT_EQ(red(std::ldexp(1.0, -21) * (1.0 + std::ldexp(1.0, -16))), 1u);
    EXPECT_EQ(red(0.001), 0x142u);
    EXPECT_EQ(red(65504.0), 0x7bfu);
    EXPECT_EQ(red(INFINITY), 0x7c0u);
    EXPECT_EQ(red(NAN), 0x7ffu);
}

// Extended range: floor((v + 384/510) 510 + 1/2) in float steps (0.25 stores 511, not 512); the sRGB twins by their
// thresholds; BGRA10_XR each code in its lane's top 10 bits
TEST(FillTexel, ExtendedRangeIsPacked)
{
    EXPECT_EQ(texel(554, 0.25, 0.25, 0.25, 0.25), "fffdf71f");
    EXPECT_EQ(texel(554, 0, 0, 0, 0), "80010618");
    EXPECT_EQ(texel(552, 1, 1, 1, 1), "80df80df80df80df");
    EXPECT_EQ(texel(552, -1, -1, -1, -1), "0000000000000060");
    EXPECT_EQ(texel(553, 0.5, 0.5, 0.5, 0.5), "c0bdc0bdc0bdc09f");
    EXPECT_EQ(texel(555, -0.5, -0.5, -0.5, -0.5), "09249000");
    EXPECT_EQ(texel(555, NAN, NAN, NAN, NAN), "80010618");
}

// A format whose fills do not follow the colour (compressed, depth, stencil, two-plane) has no texel
TEST(FillTexel, FormatsWhoseFillsDoNotFollowTheColourHaveNone)
{
    for (Format format : {250u, 252u, 253u, 260u, 550u, 130u, 204u})
        EXPECT_EQ(texel(format, 0.5, 0.5, 0.5, 0.5), "") << format;
}

}  // namespace
