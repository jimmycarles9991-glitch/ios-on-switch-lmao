// SPDX-License-Identifier: MIT
// What Metal's blits move of a texture held through planes (blit_planes.h), as an M4 Max under macOS 27.2.1 moves it
// with validation off: plane 0's bytes, planeUnit of them a texel
#include "../blit_planes.h"

#include <gtest/gtest.h>
#include <vector>

namespace {

struct Planes {
    gm20b::Import import;
    gm20b::Texture luma, t;

    Planes()
    {
        import.id = 7;
        luma.import = &import;
        luma.va = 0x10000;
        luma.pitch = 64;
        luma.height = 8;
        t.width = 24;
        t.height = 8;
    }
};

TEST(BlitPlanes, EachKindCountsPlaneZeroInItsUnit)
{
    Planes eight, ten, packed, one, xr, planar;
    eight.luma.format = 10;   // R8Unorm codes
    ten.luma.format = 20;     // R16Unorm codes
    packed.luma.format = 53;  // packed 10-bit words
    for (Planes *p : {&eight, &ten, &packed}) {
        p->t.ycbcr = 1;
        p->t.luma = &p->luma;
    }
    one.t.ycbcr = 1;
    one.t.onePlane = true;
    one.t.import = &one.import;
    one.t.planes = {0x20000, 0, 128, 0};
    xr.t.xrPlanes = true;
    xr.t.luma = &xr.luma;
    planar.t.planar = true;
    planar.t.import = &planar.import;
    planar.t.planes = {0x30000, 0x40000, 32, 64};
    EXPECT_EQ(gm20b::planeUnit(eight.t), 1u);
    EXPECT_EQ(gm20b::planeUnit(ten.t), 2u);
    EXPECT_EQ(gm20b::planeUnit(packed.t), 1u);
    EXPECT_EQ(gm20b::planeUnit(one.t), 2u);
    EXPECT_EQ(gm20b::planeUnit(xr.t), 4u);
    EXPECT_EQ(gm20b::planeUnit(planar.t), 1u);
    EXPECT_EQ(gm20b::planeUnit(gm20b::Texture()), 0u);

    gm20b::Texture out;
    ASSERT_TRUE(gm20b::planeBytes(packed.t, 1, out));
    EXPECT_EQ(out.va, 0x10000u);
    EXPECT_EQ(out.pitch, 64u);
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.format, 10u);
    EXPECT_FALSE(out.blockLinear);
    ASSERT_TRUE(gm20b::planeBytes(one.t, 2, out));
    EXPECT_EQ(out.va, 0x20000u);
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.bpp, 2u);
    ASSERT_TRUE(gm20b::planeBytes(xr.t, 16, out));  // counted in a plain RGBA32 texture's texel
    EXPECT_EQ(out.width, 4u);
    EXPECT_EQ(out.format, 123u);
    EXPECT_EQ(out.storage.bytes, 64u * 7 + 64u);
    EXPECT_FALSE(gm20b::planeBytes(xr.t, 3, out));
    EXPECT_FALSE(gm20b::planeBytes(xr.t, 128, out));
}

// RGB10A8_2P_XR10's words over a surface the kernel laid out block-linear (an M4 Max makes 550 over a planeless 'w30r' a
// BGR10_XR target was cleared in, and Metal's validation stops a texture copy of 550 with any other format): plane 0 is
// its own texels in that layout, a region inside them
TEST(BlitPlanes, ABlockLinearWordPlaneIsItsTexelsInItsLayout)
{
    Planes xr;
    xr.t.xrPlanes = true;
    xr.t.luma = &xr.luma;
    xr.luma.width = 16;
    xr.luma.bpp = 4;
    xr.luma.blockLinear = true;
    xr.luma.blockHeight = 1;
    xr.luma.layerBytes = 4096;
    xr.luma.surfaceX = 2;
    xr.luma.surfaceY = 3;
    gm20b::Texture out;
    ASSERT_TRUE(gm20b::planeBytes(xr.t, 4, out));
    EXPECT_TRUE(out.blockLinear);
    EXPECT_EQ(out.va, 0x10000u);
    EXPECT_EQ(out.pitch, 64u);
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 8u);
    EXPECT_EQ(out.format, 53u);
    EXPECT_EQ(out.blockHeight, 1u);
    EXPECT_EQ(out.surfaceX, 2u);
    EXPECT_EQ(out.surfaceY, 3u);
    EXPECT_TRUE(gm20b::planeRegionReadable(out, 8, 0, 8, 8));
    EXPECT_FALSE(gm20b::planeRegionReadable(out, 9, 0, 8, 8)) << "past its row's texels: no bytes of the layout";
    ASSERT_TRUE(gm20b::planeBytes(xr.t, 1, out));
    EXPECT_EQ(out.width, 64u);
    EXPECT_EQ(out.surfaceX, 8u);
    EXPECT_FALSE(gm20b::planeBytes(xr.t, 16, out)) << "its first texel off a 16-byte unit";
}

struct Piece {
    uint32_t x, row, w, at;

    bool operator==(const Piece &o) const
    {
        return x == o.x && row == o.row && w == o.w && at == o.at;
    }
};

std::vector<Piece> pieces(const gm20b::Texture &plane, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool &fits)
{
    std::vector<Piece> out;
    fits = gm20b::planeRunPieces(plane, x, y, w, h, [&](const gm20b::PlaneRunPiece &p) {
        out.push_back({p.x, p.row, p.w, p.at});
    });
    return out;
}

// A one-plane 4:4:4 'v410' (504) over a surface the kernel laid out block-linear, its rows of exactly 16 words, copied
// into a plain RGBA16Uint and RGBA32Uint texture (HW Test gpu.gm20b.planes_blits c_504_exact_*): an M4 Max reads each
// row of the region as one run of bytes along the surface's rows, so the run goes on into the next rows
TEST(BlitPlanes, ABlockLinearPlaneRunsOnIntoItsNextRows)
{
    Planes v410;
    v410.t.ycbcr = 1;
    v410.t.luma = &v410.luma;
    v410.luma.format = 90;  // RGB10A2Unorm: plane 0's words
    v410.luma.width = 16;
    v410.luma.bpp = 4;
    v410.luma.blockLinear = true;
    v410.luma.rowsPitch = 64;
    gm20b::Texture out;
    bool fits = false;

    ASSERT_TRUE(gm20b::planeBytes(v410.t, 8, out));
    EXPECT_EQ(out.width, 8u);
    EXPECT_EQ(out.rowsPitch, 64u);
    EXPECT_EQ(pieces(out, 2, 2, 8, 4, fits), (std::vector<Piece>{{2, 0, 6, 0}, {0, 1, 2, 6}}));
    EXPECT_TRUE(fits) << "bytes 16-79 of rows 2-5: the last ends 16 bytes into row 6";
    EXPECT_TRUE(gm20b::planeRegionReadable(out, 2, 2, 8, 4));

    ASSERT_TRUE(gm20b::planeBytes(v410.t, 16, out));
    EXPECT_EQ(pieces(out, 2, 2, 8, 4, fits), (std::vector<Piece>{{2, 0, 2, 0}, {0, 1, 4, 2}, {0, 2, 2, 6}}));
    EXPECT_TRUE(fits) << "bytes 32-159 of rows 2-5: the last ends in row 7";
    EXPECT_EQ(pieces(out, 1, 1, 2, 7, fits), (std::vector<Piece>{{1, 0, 2, 0}}));
    EXPECT_TRUE(fits) << "inside each row: one piece";
    EXPECT_TRUE(pieces(out, 2, 3, 8, 4, fits).empty());
    EXPECT_FALSE(fits) << "row 6's run ends in row 9, past the plane's 8";
    EXPECT_FALSE(gm20b::planeRegionReadable(out, 2, 3, 8, 4));
}

// The run wraps at the surface's rows as the CPU holds them, never at the layout's GOB-wide row: 24 words a row are 96
// bytes, laid out 128 apart; a row's padding past its texels is no byte of the layout
TEST(BlitPlanes, ABlockLinearRunWrapsAtTheSurfacesRows)
{
    Planes wide;
    wide.t.ycbcr = 1;
    wide.t.luma = &wide.luma;
    wide.luma.format = 90;
    wide.luma.width = 24;
    wide.luma.bpp = 4;
    wide.luma.pitch = 128;
    wide.luma.blockLinear = true;
    wide.luma.rowsPitch = 96;
    gm20b::Texture out;
    bool fits = false;
    ASSERT_TRUE(gm20b::planeBytes(wide.t, 16, out));
    EXPECT_EQ(out.width, 6u);
    EXPECT_EQ(pieces(out, 4, 0, 4, 2, fits), (std::vector<Piece>{{4, 0, 2, 0}, {0, 1, 2, 2}}));
    EXPECT_TRUE(fits);

    wide.luma.rowsPitch = 128;  // rows padded past their 96 bytes
    ASSERT_TRUE(gm20b::planeBytes(wide.t, 16, out));
    EXPECT_TRUE(pieces(out, 4, 0, 4, 2, fits).empty());
    EXPECT_FALSE(fits) << "bytes 96-127 of row 0 are its padding";

    wide.luma.rowsPitch = 96;
    wide.luma.surfaceY = 2;
    ASSERT_TRUE(gm20b::planeBytes(wide.t, 16, out));
    EXPECT_FALSE(gm20b::planeRegionReadable(out, 4, 0, 4, 2)) << "a crop's runs reach rows outside it";
    EXPECT_TRUE(gm20b::planeRegionReadable(out, 0, 0, 4, 2));
}

// Plane 1 as a blit between two textures held through planes moves it: its bytes from its first at the rows of plane 0
// the caller names, to the end of its surface's last page; none for a one-plane format or a plane alone's alpha
TEST(BlitPlanes, PlaneOneIsItsBytesAtPlaneZerosRows)
{
    gm20b::Import import;
    import.id = 7;
    import.bo.va = 0x10000;
    import.bo.bytes = 0x4000;
    gm20b::Texture luma, chroma, t;
    luma.import = chroma.import = &import;
    luma.va = 0x10000;
    luma.pitch = 48;
    luma.height = 8;
    luma.format = 10;
    chroma.va = 0x10400;
    chroma.pitch = 32;
    chroma.width = 8;
    chroma.height = 4;
    chroma.bpp = 2;
    t.ycbcr = 1;
    t.luma = &luma;
    t.chroma = &chroma;
    gm20b::Texture out;
    ASSERT_TRUE(gm20b::planeOneRows(t, luma.pitch, out));
    EXPECT_EQ(out.va, 0x10400u);
    EXPECT_EQ(out.pitch, 48u) << "plane 0's rows, never its own";
    EXPECT_EQ(out.bpp, 1u);
    EXPECT_EQ(out.storage.va, 0x10400u);
    EXPECT_EQ(out.storage.bytes, 0x4000u - 0x400u) << "on through the plane's end to its surface's last page";

    chroma.format = 53;  // a packed plane: bytes too, never whole words
    ASSERT_TRUE(gm20b::planeOneRows(t, luma.pitch, out));
    EXPECT_EQ(out.bpp, 1u);

    gm20b::Texture xr, alpha = chroma;
    alpha.bpp = 1;
    xr.xrPlanes = true;
    xr.luma = &luma;
    xr.chroma = &alpha;
    ASSERT_TRUE(gm20b::planeOneRows(xr, 128, out));
    EXPECT_EQ(out.pitch, 128u);
    alpha.memoryless = true;
    EXPECT_FALSE(gm20b::planeOneRows(xr, 128, out)) << "a plane alone's alpha";

    gm20b::Texture one;
    one.ycbcr = 1;
    one.onePlane = true;
    one.import = &import;
    EXPECT_FALSE(gm20b::planeOneRows(one, 64, out));
}

// The runs of a plane-1 copy whose rows end past either side's last page: an M4 Max moves the rows inside both, reads 0
// past the source's page and lands nothing past the destination's (probes/f1.txt A7), a row an end cuts byte by byte
TEST(BlitPlanes, PlaneOneRunsReadZeroPastTheSourceAndDropPastTheDestination)
{
    struct Run {
        uint32_t row, rows, copied, zeroed;

        bool operator==(const Run &o) const
        {
            return row == o.row && rows == o.rows && copied == o.copied && zeroed == o.zeroed;
        }
    };

    const auto runs = [](uint64_t readable, uint64_t writable) {
        std::vector<Run> out;
        // 16 x 8 from (8, 0) at rows 128 apart into (6, 0) at rows 128 apart
        gm20b::planeOneRuns(readable, 128, 8, 0, writable, 128, 6, 0, 16, 8, [&](const gm20b::PlaneOneRun &r) {
            out.push_back({r.row, r.rows, r.copied, r.zeroed});
        });
        return out;
    };
    EXPECT_EQ(runs(4096, 4096), (std::vector<Run>{{0, 8, 16, 0}}));
    EXPECT_EQ(runs(256, 256), (std::vector<Run>{{0, 2, 16, 0}})) << "plane 1's 256 bytes end the page";
    EXPECT_EQ(runs(200, 4096), (std::vector<Run>{{0, 2, 16, 0}, {2, 6, 0, 16}}));
    EXPECT_EQ(runs(140, 4096), (std::vector<Run>{{0, 1, 16, 0}, {1, 1, 4, 12}, {2, 6, 0, 16}}));
    EXPECT_EQ(runs(0, 4096), (std::vector<Run>{{0, 8, 0, 16}})) << "a source plane 1 at its last page's end";
    EXPECT_EQ(runs(4096, 400), (std::vector<Run>{{0, 3, 16, 0}, {3, 1, 10, 0}})) << "row 3 runs from 390 to 406";
    EXPECT_EQ(runs(4096, 395), (std::vector<Run>{{0, 3, 16, 0}, {3, 1, 5, 0}}));
}

// A plain source of another texel size than the planes' unit, read as an M4 Max reads one with validation off: each
// row's bytes in its own layout, the unit's bytes a texel (m2v-textures11 probes/h5/copies.txt)
TEST(BlitPlanes, APlainSourceIsReadInThePlanesUnit)
{
    gm20b::Texture pitch;
    pitch.va = 0x50000;
    pitch.pitch = 64;
    pitch.width = 30;
    pitch.height = 8;
    pitch.bpp = 1;
    pitch.format = 10;
    gm20b::Texture out;
    ASSERT_TRUE(gm20b::imageInUnits(pitch, 4, out));
    EXPECT_EQ(out.bpp, 4u);
    EXPECT_EQ(out.format, 53u);
    EXPECT_EQ(out.width, 16u) << "a pitch image's whole rows";
    EXPECT_EQ(out.va, 0x50000u);
    EXPECT_TRUE(gm20b::unitRegionReadable(out, 544, 8, 0, 16, 8)) << "bytes 32-95 of rows 0-7: the last ends at 544";
    EXPECT_FALSE(gm20b::unitRegionReadable(out, 512, 8, 0, 16, 8)) << "past its memory";
    EXPECT_FALSE(gm20b::unitRegionReadable(out, 544, 9, 7, 16, 1));

    gm20b::Texture tiled = pitch;
    tiled.blockLinear = true;
    tiled.width = 32;
    tiled.bpp = 2;
    tiled.surfaceX = 6;
    ASSERT_TRUE(gm20b::imageInUnits(tiled, 4, out));
    EXPECT_EQ(out.width, 16u) << "a block-linear image's texels' bytes alone";
    EXPECT_EQ(out.surfaceX, 3u);
    EXPECT_TRUE(gm20b::unitRegionReadable(out, 0, 4, 0, 12, 8));
    EXPECT_FALSE(gm20b::unitRegionReadable(out, 0, 5, 0, 12, 8));
    tiled.surfaceX = 1;
    EXPECT_FALSE(gm20b::imageInUnits(tiled, 4, out)) << "a crop of the kernel's layout off the unit";
    EXPECT_FALSE(gm20b::imageInUnits(pitch, 3, out));
}

}  // namespace
