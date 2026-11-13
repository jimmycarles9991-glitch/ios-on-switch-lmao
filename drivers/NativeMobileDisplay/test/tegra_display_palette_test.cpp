// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraDisplayPalette.h: a gamma table whose channels differ becomes one palette word per 8-bit value, each channel its
// own curve rounded as LUT2's shared one is; a table whose channels agree stays LUT2's. And the CP_ENABLE bit reaches
// 32-bit RGB windows only, through native_planes::imageOf.
#include <gtest/gtest.h>

#include <vector>

#include "../NativePlanes.h"
#include "../TegraDisplayPalette.h"

namespace {
using namespace switch_color;

std::vector<uint32_t> table(uint32_t (*curve)(unsigned channel, unsigned point))
{
    std::vector<uint32_t> t(kGammaWords);
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned point = 0; point < kGammaPoints; ++point)
            t[channel * kGammaPoints + point] = curve(channel, point);
    return t;
}

TEST(TegraDisplayPalette, ChannelsThatDifferBecomePaletteWords)
{
    const auto t = table([](unsigned channel, unsigned point) -> uint32_t {
        const uint32_t identity = gammaIdentity(point);
        return channel == 0 ? kGammaMax - identity : channel == 1 ? identity : identity * identity / kGammaMax;
    });
    EXPECT_TRUE(channelsDiffer(t.data()));
    uint32_t palette[kPaletteWords];
    paletteOf(t.data(), palette);
    for (unsigned i = 0; i < kPaletteWords; ++i) {
        EXPECT_EQ(palette[i] & 0xff, gammaByte(kGammaMax - gammaIdentity(i))) << i;
        EXPECT_EQ(palette[i] >> 8 & 0xff, i) << i;
        EXPECT_EQ(palette[i] >> 16 & 0xff, gammaByte(gammaIdentity(i) * gammaIdentity(i) / kGammaMax)) << i;
        EXPECT_EQ(palette[i] >> 24, 0u) << i;
    }
    EXPECT_EQ(palette[0] & 0xff, 255u);
    EXPECT_LE(palette[255] & 0xff, 1u);  // 1023 - 1020 rounds to 1
}

TEST(TegraDisplayPalette, AgreeingChannelsStayInLut2)
{
    const auto t = table([](unsigned, unsigned point) -> uint32_t {
        return gammaIdentity(point) / 2;
    });
    EXPECT_FALSE(channelsDiffer(t.data()));
    auto u = t;
    u[2 * kGammaPoints + 256] += 1;  // the last point of blue alone
    EXPECT_TRUE(channelsDiffer(u.data()));
}

TEST(TegraDisplayPalette, TheWindowsAreWincAToC)
{
    EXPECT_EQ(kPaletteBase[0], 0xa00u);
    EXPECT_EQ(kPaletteBase[1], 0xc00u);
    EXPECT_EQ(kPaletteBase[2], 0xe00u);
    EXPECT_EQ(kCpEnable, 1u << 16);
}

TEST(TegraDisplayPalette, CpEnableReachesRgbWindowsOnly)
{
    native_planes::Layer rgb;
    rgb.kind = native_planes::kKindBgra;
    rgb.surface.format = native_planes::fourcc('B', 'G', 'R', 'A');
    rgb.surface.width = 512;
    rgb.surface.height = 512;
    rgb.surface.stride[0] = 2048;
    rgb.surface.allocation = rgb.surface.descriptorBytes = 2048 * 512;
    rgb.src = rgb.dst = {0, 0, 512, 512};
    native_planes::Layer yuv;
    yuv.kind = native_planes::kKindNv12Video;
    yuv.surface.format = native_planes::fourcc('4', '2', '0', 'v');
    yuv.surface.planes = 2;
    yuv.surface.width = 512;
    yuv.surface.height = 512;
    yuv.surface.stride[0] = yuv.surface.stride[1] = 512;
    yuv.surface.offset[1] = 512 * 512;
    yuv.surface.planeHeight[0] = 512;
    yuv.surface.planeHeight[1] = 256;
    yuv.surface.allocation = yuv.surface.descriptorBytes = 512 * 768;
    yuv.src = yuv.dst = {0, 0, 512, 512};
    native_planes::Image image;
    for (const uint32_t options : {0u, kCpEnable}) {
        switch_window_palette::setOptions(options);
        ASSERT_TRUE(native_planes::imageOf(rgb, 720, 1280, image));
        EXPECT_EQ(image.options & kCpEnable, options);
        ASSERT_TRUE(native_planes::imageOf(yuv, 720, 1280, image));
        EXPECT_EQ(image.options & kCpEnable, 0u);
    }
    switch_window_palette::setOptions(0);
}

}  // namespace
