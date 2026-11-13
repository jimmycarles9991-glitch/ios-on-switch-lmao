// drivers/TegraVIC/TegraVICBounce.h on the host: which surfaces need the copy-engine bounce, the scratch's geometry, and
// VIC's channel wait words.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "../../TegraHost1x/TegraHost1x.h"
#include "../TegraVICBounce.h"

namespace {
TegraVICSurfaceDesc nv12(uint32_t width, uint32_t height, uint64_t pitch)
{
    TegraVICSurfaceDesc d;
    memset(&d, 0, sizeof(d));
    d.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    d.width = width, d.height = height, d.planes = 2;
    d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = pitch;
    d.planeWidth[0] = width, d.planeWidth[1] = width / 2;
    d.planeHeight[0] = height, d.planeHeight[1] = height / 2;
    d.planeOffset[1] = (pitch * height + 16383) / 16384 * 16384;
    d.length = d.planeOffset[1] + pitch * (height / 2);
    return d;
}
}  // namespace

TEST(VicBounce, OnlyRowsVicCannotAddressBounce)
{
    const TegraVICSurfaceDesc wide = nv12(1920, 1080, 2048), tall = nv12(1080, 1920, 1088);
    EXPECT_EQ(tegra_vic_bounce_needed(&wide, 256), 1);
    EXPECT_EQ(tegra_vic_bounce_needed(&tall, 256), 0) << "CoreVideo's 64-byte rows";
    TegraVICSurfaceDesc odd = tall;
    odd.planeOffset[1] += 8;
    EXPECT_EQ(tegra_vic_bounce_needed(&odd, 256), -1) << "a plane offset the copy engine cannot take";
}

TEST(VicBounce, PlanesVicCannotAddressBounce)
{
    TegraVICSurfaceDesc d = nv12(300, 534, 320);
    d.planeOffset[1] = 320 * 534;
    EXPECT_EQ(tegra_vic_bounce_needed(&d, 64), 0) << "a chroma plane off 256 bytes";
    d.planeOffset[1] = 320 * 536;
    EXPECT_EQ(tegra_vic_bounce_needed(&d, 64), 1);
}

TEST(VicBounce, ScratchIsTheSamePixelsAtVicPitches)
{
    const TegraVICSurfaceDesc d = nv12(1080, 1920, 1088);
    TegraVICSurfaceDesc s;
    tegra_vic_bounce_desc(&d, 256, &s);
    EXPECT_EQ(s.fourcc, d.fourcc);
    EXPECT_EQ(s.width, 1080u);
    EXPECT_EQ(s.planeBytesPerRow[0], 1280u);
    EXPECT_EQ(s.planeBytesPerRow[1], 1280u) << "540 CbCr pairs, 1080 bytes";
    EXPECT_EQ(s.planeOffset[0], 0u);
    EXPECT_EQ(s.planeOffset[1], 1280ull * 1920);
    EXPECT_EQ(s.planeOffset[1] % TEGRA_VIC_BOUNCE_PAGE, 0u);
    EXPECT_EQ(s.length, s.planeOffset[1] + 1280ull * 960);
    EXPECT_EQ(s.length % TEGRA_VIC_BOUNCE_PAGE, 0u);
    EXPECT_EQ(tegra_vic_bounce_needed(&s, 256), 1);
}

TEST(VicBounce, RowBytesAreThePixelsNotThePitch)
{
    TegraVICSurfaceDesc bgra;
    memset(&bgra, 0, sizeof(bgra));
    bgra.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    bgra.width = 834, bgra.height = 600;
    bgra.planeBytesPerRow[0] = 3392;  // 834 x 4 = 3336, in 64-byte rows
    bgra.planeHeight[0] = 600;
    bgra.length = 3392ull * 600;
    const TegraVICFormat *f = tegra_vic_format(bgra.fourcc);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(f, 834, 0), 3336u);
    EXPECT_EQ(tegra_vic_bounce_needed(&bgra, 256), 0);
    TegraVICSurfaceDesc s;
    tegra_vic_bounce_desc(&bgra, 256, &s);
    EXPECT_EQ(s.planeBytesPerRow[0], 3584u);
    EXPECT_EQ(s.length, (3584ull * 600 + 4095) / 4096 * 4096);
}

TEST(VicBounceOrder, VicChannelWaitPreservesFullThresholdAndExactWordBound)
{
    for (const uint32_t threshold : {0u, 0x00ffffffu, 0x01000000u, 0x81234567u, 0xffffffffu}) {
        uint32_t words[SWITCH_HOST1X_WAIT_WORDS] = {};
        EXPECT_EQ(tegra_host1x_op_wait_syncpt(words, 37, threshold), SWITCH_HOST1X_WAIT_WORDS);
        EXPECT_EQ(words[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
        EXPECT_EQ(words[1], threshold);
        EXPECT_EQ(words[2], 37u);
    }
}
