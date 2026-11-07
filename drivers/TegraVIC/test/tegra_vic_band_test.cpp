// drivers/TegraVIC/TegraVICBand.h on the host: the rows of a 4:2:0 surface a placed picture covers (a grid image's
// tile), the pages they touch, and the runs that cover exactly the rectangle's bytes, page by page.
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "../TegraVICBand.h"

namespace {
struct Span {
    uint32_t page, offset, length;
};

void collect(void *context, uint32_t page, uint32_t offset, uint32_t length)
{
    static_cast<std::vector<Span> *>(context)->push_back({page, offset, length});
}

std::vector<Span> runs(const TegraVICBand &b)
{
    std::vector<Span> out;
    tegra_vic_band_runs(&b, collect, &out);
    return out;
}

// Every byte the runs name, as offsets from the band's first page.
std::set<uint64_t> bytes(const std::vector<Span> &rs)
{
    std::set<uint64_t> out;
    for (const Span &r : rs)
        for (uint32_t i = 0; i < r.length; i++)
            out.insert((uint64_t)r.page * TEGRA_VIC_BAND_PAGE + r.offset + i);
    return out;
}
}  // namespace

TEST(VicBand, LumaAndChromaOfATile)
{
    // An 8064x6048 canvas, pitch 8192, chroma after 6048 rows; the tile at (512, 1024), 512x512.
    const uint64_t chromaOffset = 8192ull * 6048;
    const TegraVICBand luma = tegra_vic_band_420(0, 8192, 0, 512, 1024, 512, 512);
    const TegraVICBand chroma = tegra_vic_band_420(chromaOffset, 8192, 1, 512, 1024, 512, 512);
    EXPECT_EQ(luma.start, 1024ull * 8192 + 512);
    EXPECT_EQ(luma.rows, 512u);
    EXPECT_EQ(luma.rowBytes, 512u);
    EXPECT_EQ(chroma.start, chromaOffset + 512ull * 8192 + 512);
    EXPECT_EQ(chroma.rows, 256u);
    EXPECT_EQ(chroma.rowBytes, 512u);
    EXPECT_EQ(tegra_vic_band_end(&luma), luma.start + 511ull * 8192 + 512);
    EXPECT_EQ(tegra_vic_band_pages(&luma), 1023u) << "two pages a row, but only the last row's first";
    EXPECT_EQ(tegra_vic_band_pages(&chroma), 511u);
}

TEST(VicBand, RunsCoverExactlyTheRectangle)
{
    const TegraVICBand b = tegra_vic_band_420(0, 1280, 0, 256, 6, 300, 40);
    const std::vector<Span> rs = runs(b);
    const std::set<uint64_t> got = bytes(rs);
    std::set<uint64_t> want;
    const uint64_t base = b.start / TEGRA_VIC_BAND_PAGE * TEGRA_VIC_BAND_PAGE;
    for (uint32_t r = 0; r < b.rows; r++)
        for (uint32_t x = 0; x < b.rowBytes; x++)
            want.insert(b.start + (uint64_t)r * b.pitch + x - base);
    EXPECT_EQ(got, want);
    uint64_t total = 0;
    for (const Span &r : rs) {
        EXPECT_LE(r.offset + r.length, TEGRA_VIC_BAND_PAGE) << "a run inside one page";
        EXPECT_LT(r.page, tegra_vic_band_pages(&b));
        total += r.length;
    }
    EXPECT_EQ(total, 300ull * 40) << "no byte twice";
}

TEST(VicBand, WholeFrameIsEveryRow)
{
    // A 1920x1080 picture at the corner of a 2048-pitch pool buffer: the band is the whole luma plane but the padding.
    const TegraVICBand b = tegra_vic_band_420(0, 2048, 0, 0, 0, 1920, 1080);
    EXPECT_EQ(b.start, 0u);
    EXPECT_EQ(tegra_vic_band_end(&b), 1079ull * 2048 + 1920);
    EXPECT_EQ(tegra_vic_band_pages(&b), (uint32_t)((1079ull * 2048 + 1920 + 4095) / 4096));
    EXPECT_EQ(bytes(runs(b)).size(), 1920ull * 1080);
}

TEST(VicBand, EmptyRectangleHasNoRuns)
{
    const TegraVICBand b = tegra_vic_band_420(4096, 256, 1, 0, 0, 2, 0);
    EXPECT_EQ(b.rows, 0u);
    EXPECT_TRUE(runs(b).empty());
    EXPECT_EQ(tegra_vic_band_end(&b), b.start);
}
