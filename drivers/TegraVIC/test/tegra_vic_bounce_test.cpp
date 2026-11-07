// drivers/TegraVIC/TegraVICBounce.h on the host: which surfaces need the copy-engine bounce, the scratch's geometry, and
// how the copy engine and VIC order each other (the fences each waits for, and VIC's channel wait words).
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

TEST(VicBounceOrder, VicWaitsForTheLastFillAndTheCopyBackFinishesTheJob)
{
    uint32_t next = 100;
    const TegraVICBounceOrder both = tegra_vic_bounce_order(1, 1, &next);
    EXPECT_EQ(both.fills, 2u);
    EXPECT_TRUE(both.vicWaits);
    EXPECT_TRUE(both.copiesBack);
    EXPECT_EQ(both.vicAcquire, 102u) << "only the last fill signals: one release, two increments";
    EXPECT_EQ(both.done, 104u);
    EXPECT_EQ(next, 104u);

    const TegraVICBounceOrder source = tegra_vic_bounce_order(1, 0, &next);
    EXPECT_EQ(source.fills, 1u);
    EXPECT_TRUE(source.vicWaits);
    EXPECT_FALSE(source.copiesBack) << "VIC's own fence finishes the job";
    EXPECT_EQ(source.vicAcquire, 106u);
    EXPECT_EQ(next, 106u);

    const TegraVICBounceOrder destination = tegra_vic_bounce_order(0, 1, &next);
    EXPECT_EQ(destination.fills, 1u) << "the destination's own pixels, for those VIC leaves";
    EXPECT_EQ(destination.vicAcquire, 108u);
    EXPECT_EQ(destination.done, 110u);

    const TegraVICBounceOrder none = tegra_vic_bounce_order(0, 0, &next);
    EXPECT_FALSE(none.vicWaits);
    EXPECT_FALSE(none.copiesBack);
    EXPECT_EQ(next, 110u) << "a job without a bounce queues no copy and moves no fence";
}

TEST(VicBounceOrder, FencesWrapWithTheSyncpoint)
{
    uint32_t next = 0xfffffffeu;
    const TegraVICBounceOrder o = tegra_vic_bounce_order(1, 1, &next);
    EXPECT_EQ(o.vicAcquire, 0u);
    EXPECT_EQ(o.done, 2u);
}

TEST(VicBounceOrder, VicChannelWaitWords)
{
    uint32_t w[2];
    EXPECT_EQ(tegra_host1x_op_wait_syncpt(w, 37, 0x1234567), 2u);
    EXPECT_EQ(w[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 8, 1)) << "host1x class, WAIT_SYNCPT";
    EXPECT_EQ(w[1], (37u << 24) | 0x234567u) << "index in 31:24, threshold in 23:0";
}
