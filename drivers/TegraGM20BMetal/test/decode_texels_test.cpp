// SPDX-License-Identifier: MIT
// The decoders the pack's decode states compile (pack/decode.metal: astc_decode.h, pvrtc_decode.h) against what an M4
// Max's GPU samples from the same blocks (decode_vectors.h): ASTC blocks of every partition count, dual plane, colour
// endpoint mode, void extent and the error colour in Metal's 4x2 and 8x4 footprints and in HDR, and whole PVRTC levels of
// both block sizes, with and without alpha, down to one texel. And decoded.h's runtime side against them: a level's
// Morton bytes into rows, the step's forms and what a never-written texel reads.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "../pack/astc_decode.h"
#include "../pack/pvrtc_decode.h"
#include "../decoded.h"
#include "decode_vectors.h"

namespace {

using namespace gm20b::decode;

int formOf(uint32_t format)
{
    return format >= 221 ? kAstcHalf : format <= 200 ? kAstcUnorm8 : kAstcUnorm12;
}

bool halfNan(uint32_t h)
{
    return (h & 0x7c00) == 0x7c00 && (h & 0x3ff);
}

struct Morton {
    const uint32_t *words;
    uint32_t blocksX, blocksY;
};

Words2 blockAt(Morton m, int x, int y)
{
    const uint32_t i = gm20b::pvrtcMortonBlock(uint32_t(x), uint32_t(y), m.blocksX, m.blocksY);
    return {m.words[2 * i], m.words[2 * i + 1]};
}

struct Rows {
    const uint8_t *bytes;
    uint32_t blocksX;
};

Words2 blockAt(Rows r, int x, int y)
{
    Words2 w;
    memcpy(&w, r.bytes + 8 * (uint64_t(y) * r.blocksX + uint64_t(x)), 8);
    return w;
}

TEST(DecodeTexels, AstcTexelsAreWhatAppleSamples)
{
    for (const AstcVector &v : kAstcVectors) {
        const Words4 b = {v.words[0], v.words[1], v.words[2], v.words[3]};
        const Texel t = astcTexel(b, int(v.blockW), int(v.blockH), int(v.x), int(v.y), formOf(v.format));
        const uint32_t got[4] = {t.r, t.g, t.b, t.a};
        for (int c = 0; c < 4; c++) {
            if (formOf(v.format) == kAstcHalf && halfNan(v.want[c])) {
                EXPECT_TRUE(halfNan(got[c])) << v.format << " " << v.exercises << " channel " << c;
                continue;
            }
            EXPECT_EQ(got[c], v.want[c]) << "format " << v.format << " " << v.exercises << " texel " << v.x << ","
                                         << v.y << " channel " << c << " block " << std::hex << v.words[0];
        }
    }
}

// The PVRTC levels' blocks lie in Morton order, y in the even bits: read in another order, nearly every texel differs
TEST(DecodeTexels, PvrtcLevelsAreWhatAppleSamples)
{
    for (const PvrtcVector &v : kPvrtcVectors) {
        const bool twoBpp = (v.format - 160) / 2 % 2 == 0;
        const uint32_t blocksX = gm20b::decodedBlocks(v.side, twoBpp ? 8 : 4),
                       blocksY = gm20b::decodedBlocks(v.side, 4);
        const Morton m = {v.blocks, blocksX, blocksY};
        for (uint32_t y = 0; y < v.side; y++)
            for (uint32_t x = 0; x < v.side; x++) {
                const Texel t = pvrtcTexel(m, int(x), int(y), int(blocksX), int(blocksY), twoBpp);
                const uint32_t alpha = gm20b::decodedFormat(v.format)->opaque ? 0xff : t.a;
                EXPECT_EQ(t.r | t.g << 8 | t.b << 16 | alpha << 24, v.want[y * v.side + x])
                    << "format " << v.format << " side " << v.side << " texel " << x << "," << y;
            }
    }
}

// A level's replaceRegion bytes put into rows (decoded.h pvrtcRows, as replaceRegion stages them) decode as the
// level's Morton bytes do: the decode states read rows
TEST(DecodeTexels, APvrtcLevelInRowsDecodesAsItsMortonBytes)
{
    for (const PvrtcVector &v : kPvrtcVectors) {
        const bool twoBpp = (v.format - 160) / 2 % 2 == 0;
        const uint32_t blocksX = gm20b::decodedBlocks(v.side, twoBpp ? 8 : 4),
                       blocksY = gm20b::decodedBlocks(v.side, 4);
        std::vector<uint8_t> rows(8 * blocksX * blocksY);
        gm20b::pvrtcRows(reinterpret_cast<const uint8_t *>(v.blocks), blocksX, blocksY, rows.data());
        const Rows r = {rows.data(), blocksX};
        const Morton m = {v.blocks, blocksX, blocksY};
        for (uint32_t y = 0; y < v.side; y++)
            for (uint32_t x = 0; x < v.side; x++) {
                const Texel a = pvrtcTexel(r, int(x), int(y), int(blocksX), int(blocksY), twoBpp),
                            b = pvrtcTexel(m, int(x), int(y), int(blocksX), int(blocksY), twoBpp);
                EXPECT_TRUE(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a) << v.format << " " << x << "," << y;
            }
    }
}

// The linear 4x2 and 8x4 formats' texels: Apple's 12-bit k / 4095 held as its nearest 16-bit code, at most half a code
// from Apple's value, so no texel of an 8-bit target rounds otherwise. The owner chose RGBA16Unorm (half RGBA32Float's
// memory) over Apple's exact floats, accepting that difference.
TEST(DecodeTexels, LinearTexelsAreTheNearest16BitCodes)
{
    for (uint32_t k = 0; k <= 4095; k++) {
        const uint32_t code = unorm12To16(k);
        const double exact = double(k) * 65535.0 / 4095.0;
        EXPECT_LE(std::abs(double(code) - exact), 0.5) << k;
        EXPECT_EQ(std::lround(double(code) / 65535.0 * 255.0), std::lround(double(k) / 4095.0 * 255.0)) << k;
    }
    EXPECT_EQ(unorm12To16(4095), 65535u);
}

// What a texture of a decoded format reads before anything is uploaded: its blocks of zeros' decode, in its stored format
TEST(DecodeTexels, ANeverWrittenTexelIsItsZeroBlocksDecode)
{
    for (const gm20b::DecodedFormat &f : gm20b::kDecodedFormats) {
        uint32_t want[4] = {};
        if (f.state == gm20b::DecodeState::Pvrtc) {
            const uint32_t zeros[2] = {};
            const Morton m = {zeros, 1, 1};
            const Texel t = pvrtcTexel(m, 0, 0, 1, 1, f.blockW == 8);
            want[0] = t.r | t.g << 8 | t.b << 16 | (f.opaque ? 0xffu : t.a) << 24;
        } else {
            const Texel t = astcTexel({0, 0, 0, 0}, f.blockW, f.blockH, 0, 0, formOf(f.format));
            if (f.state == gm20b::DecodeState::AstcUnorm8) {
                want[0] = t.r | t.g << 8 | t.b << 16 | t.a << 24;
            } else if (f.state == gm20b::DecodeState::AstcUnorm16) {
                want[0] = unorm12To16(t.r) | unorm12To16(t.g) << 16, want[1] = unorm12To16(t.b) | unorm12To16(t.a)
                                                                                                      << 16;
            } else {
                want[0] = t.r | t.g << 16, want[1] = t.b | t.a << 16;
            }
        }
        EXPECT_EQ(memcmp(want, f.fresh, sizeof want), 0) << f.format;
    }
}

// The step's form is the decode state's: astc_texel.h's forms, and a PVRTC format without alpha's 1
TEST(DecodeTexels, TheStepCarriesEachFormatsForm)
{
    for (const gm20b::DecodedFormat &f : gm20b::kDecodedFormats) {
        const gm20b::DecodeStep s = gm20b::decodeStep(f, 0, 0, 16, 16, 64, 0);
        if (f.state == gm20b::DecodeState::Pvrtc)
            EXPECT_EQ(s.form, f.opaque ? 1u : 0u) << f.format;
        else
            EXPECT_EQ(int(s.form), formOf(f.format)) << f.format;
    }
}

// A texture's kept blocks: each level after the one above, each its planes (layers, or a 3D level's depth planes), each
// its rows of whole blocks, a level ending inside a block holding that block
TEST(DecodeTexels, KeptBlocksLieLevelAfterLevel)
{
    const gm20b::DecodedFormat &narrow = *gm20b::decodedFormat(203);  // 4x2, 16 bytes a block
    const gm20b::KeptLevel l0 = gm20b::keptLevel(narrow, 30, 10, 1, 3, 0),
                           l1 = gm20b::keptLevel(narrow, 30, 10, 1, 3, 1);
    EXPECT_EQ(l0.offset, 0u);
    EXPECT_EQ(l0.rowBytes, 8u * 16);  // 30 texels: 8 blocks
    EXPECT_EQ(l0.planeBytes, 8u * 16 * 5);
    EXPECT_EQ(l0.planes, 3u);
    EXPECT_EQ(l1.offset, l0.planeBytes * 3);
    EXPECT_EQ(l1.rowBytes, 4u * 16);        // 15 texels: 4 blocks
    EXPECT_EQ(l1.planeBytes, 4u * 16 * 3);  // 5 rows: 3 blocks
    EXPECT_EQ(gm20b::keptBytes(narrow, 30, 10, 1, 3, 2), l1.offset + l1.planeBytes * 3);
    const gm20b::DecodedFormat &hdr = *gm20b::decodedFormat(222);  // a 3D texture: a level's planes halve with it
    EXPECT_EQ(gm20b::keptLevel(hdr, 16, 16, 8, 1, 0).planes, 8u);
    EXPECT_EQ(gm20b::keptLevel(hdr, 16, 16, 8, 1, 1).planes, 4u);
    EXPECT_EQ(gm20b::keptLevel(hdr, 16, 16, 8, 1, 1).offset, 4u * 4 * 16 * 8);
}

// The views an M4 Max makes of these textures as another form (sRGB/linear of every texture, LDR/HDR of one footprint),
// and which hold an image of their own: every ASTC one, never PVRTC's sRGB/linear pair, and none of a texture GM20B
// decodes itself, which keeps no blocks
TEST(DecodeTexels, OwnFormsAreAnotherAstcFormOfOneFootprint)
{
    for (const auto &[source, view] :
         {std::pair{203u, 185u}, std::pair{185u, 203u}, std::pair{209u, 191u}, std::pair{203u, 221u},
          std::pair{221u, 185u}, std::pair{222u, 204u}, std::pair{222u, 186u}, std::pair{236u, 218u}})
        EXPECT_TRUE(gm20b::decodedOwnForm(source, view)) << source << " as " << view;
    for (const auto &[source, view] : {std::pair{166u, 167u}, std::pair{204u, 222u}, std::pair{186u, 222u},
                                       std::pair{203u, 203u}, std::pair{203u, 204u}, std::pair{221u, 222u}})
        EXPECT_FALSE(gm20b::decodedOwnForm(source, view)) << source << " as " << view;
    EXPECT_TRUE(gm20b::decodedViewShared(166, 167)) << "PVRTC's sRGB form reads the same codes: one image, two headers";
    EXPECT_FALSE(gm20b::keepsBlocks(166));
    EXPECT_TRUE(gm20b::keepsBlocks(185) && gm20b::keepsBlocks(203) && gm20b::keepsBlocks(236));
}

}  // namespace
