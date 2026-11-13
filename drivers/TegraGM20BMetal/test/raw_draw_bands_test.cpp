// SPDX-License-Identifier: MIT
// raw/runtime/raw_draw_bands.h on the host: a draw's bands cover its scissor area once in every turn of the plane, the
// banded word stream draws each pixel the whole draw draws exactly once and only under its scissor, puts the app's
// scissor back, writes no report or clear between bands, and a band count follows the budget.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../raw/runtime/raw_draw_bands.h"

namespace {

using raw::BandDraw;
using raw::PixelBox;
using raw::Rect;
using raw::Rotation;

Rotation turned(uint8_t turn, uint32_t w, uint32_t h)
{
    Rotation r;
    r.turn = turn;
    r.width = w;
    r.height = h;
    return r;
}

uint64_t milliseconds(double ms)
{
    return uint64_t(ms * 1e6);
}

TEST(BandRect, TheBandsCoverTheAreaOnceWhateverTheBox)
{
    const PixelBox area = {16, 8, 300, 200};
    const PixelBox boxes[] = {
        {0, 0, 1280, 720}, {40, 50, 90, 120}, {16, 8, 300, 200}, {0, 300, 10, 400}, {20, 190, 30, 199}};
    for (const PixelBox &box : boxes)
        for (uint32_t n = 1; n <= raw::kMaxBands; n++) {
            uint32_t next = area.y0;
            for (uint32_t i = 0; i < n; i++) {
                const PixelBox b = raw::bandRect(area, box, i, n);
                EXPECT_EQ(b.x0, area.x0);
                EXPECT_EQ(b.x1, area.x1);
                EXPECT_EQ(b.y0, next) << "band " << i << " of " << n;
                EXPECT_LE(b.y0, b.y1);
                next = b.y1;
            }
            EXPECT_EQ(next, area.y1);
        }
}

TEST(BandRect, TheBoxRowsAreSharedEvenly)
{
    const PixelBox area = {0, 0, 1280, 720}, box = {0, 0, 1280, 720};
    for (uint32_t i = 0; i < 12; i++) {
        const PixelBox b = raw::bandRect(area, box, i, 12);
        EXPECT_EQ(b.y1 - b.y0, 60u);
    }
}

TEST(BandRect, EveryTurnOfThePlaneIsCoveredOnce)
{
    const uint32_t w = 40, h = 24;
    const PixelBox area = {3, 2, 37, 21}, box = {5, 4, 30, 18};
    for (uint8_t turn = 0; turn <= 4; turn++) {
        const Rotation rot = turned(turn, w, h);
        std::vector<int> hits(size_t(rot.planeWidth()) * rot.planeHeight());
        const uint32_t n = 5;
        for (uint32_t i = 0; i < n; i++) {
            const PixelBox b = raw::bandRect(area, box, i, n);
            const Rect s = rot.rect({b.x0, b.y0, b.x1, b.y1});
            for (uint32_t y = s.y0; y < s.y1; y++)
                for (uint32_t x = s.x0; x < s.x1; x++)
                    hits[size_t(y) * rot.planeWidth() + x]++;
        }
        const Rect whole = rot.rect({area.x0, area.y0, area.x1, area.y1});
        for (uint32_t y = 0; y < rot.planeHeight(); y++)
            for (uint32_t x = 0; x < rot.planeWidth(); x++) {
                const bool inside = x >= whole.x0 && x < whole.x1 && y >= whole.y0 && y < whole.y1;
                EXPECT_EQ(hits[size_t(y) * rot.planeWidth() + x], inside ? 1 : 0)
                    << "turn " << int(turn) << " at " << x << "," << y;
            }
    }
}

TEST(BandCount, ADrawWithinTheBudgetIsWhole)
{
    BandDraw d;
    d.fragmentNs = milliseconds(0.9);
    d.rows = 720;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 1u);
}

// An 11.5 ms full-screen draw in 1 ms bands; a 40 ms one in 40; a 100 ms one in as many 16-row bands as 720 rows hold
TEST(BandCount, ALongDrawGoesInBandsOfTheBudget)
{
    BandDraw d;
    d.fragmentNs = milliseconds(11.5);
    d.rows = 720;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 12u);
    d.fragmentNs = milliseconds(40);
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 40u);
    d.fragmentNs = milliseconds(100);
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 720u / raw::kMinBandRows);
    d.rows = 4096;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), raw::kMaxBands);
}

TEST(BandCount, BandsStayAtLeastTheMinimumRowsTall)
{
    BandDraw d;
    d.fragmentNs = milliseconds(11.5);
    d.rows = 48;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 48u / raw::kMinBandRows);
    d.rows = raw::kMinBandRows;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 1u);
}

TEST(BandCount, RepeatedVertexWorkStaysUnderAnEighthOfThePixels)
{
    BandDraw d;
    d.fragmentNs = milliseconds(8);
    d.rows = 720;
    d.vertexNs = d.fragmentNs / 8 / 3;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 4u);
    d.vertexNs = d.fragmentNs;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, raw::kBandsPerCommandBuffer), 1u);
}

TEST(BandCount, TheCommandBuffersShareAndANoBudgetNeverBands)
{
    BandDraw d;
    d.fragmentNs = milliseconds(11.5);
    d.rows = 720;
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, 3), 3u);
    EXPECT_EQ(raw::bandCount(d, TGM_APP_BAND_NS, 0), 1u);
    EXPECT_EQ(raw::bandCount(d, 0, raw::kBandsPerCommandBuffer), 1u);
}

TEST(WritesMemory, TheShaderHeadersGlobalStoreBit)
{
    const uint32_t stores[1] = {1u << 16 | 0x62}, none[1] = {0x62};
    EXPECT_TRUE(raw::writesMemory(reinterpret_cast<const uint8_t *>(stores)));
    EXPECT_FALSE(raw::writesMemory(reinterpret_cast<const uint8_t *>(none)));
    EXPECT_TRUE(raw::writesMemory(nullptr));
}

// The band stream read back: per draw (its END or an MME draw call) the pixels of `footprint` (the triangles' pixels,
// which a draw's box may understate) inside the scissor of the moment are drawn once more.
struct Raster {
    Rotation rot;
    Rect footprint;
    std::vector<int> hits;
    uint32_t scissorH = 0, scissorV = 0, draws = 0, cuts = 0;
    std::vector<uint16_t> others;

    void run(const std::vector<uint32_t> &words)
    {
        std::vector<switch_gm20b_method> m(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        ASSERT_GE(n, 0);
        hits.assign(size_t(rot.planeWidth()) * rot.planeHeight(), 0);
        for (long i = 0; i < n; i++) {
            const uint16_t method = m[size_t(i)].method;
            if (method == NVB197_SET_SCISSOR_HORIZONTAL(0)) {
                scissorH = m[size_t(i)].value;
            } else if (method == NVB197_SET_SCISSOR_VERTICAL(0)) {
                scissorV = m[size_t(i)].value;
            } else if (method == NVB197_END) {
                draw();
            } else if (method == NVB197_NO_OPERATION) {
                cuts++;
            } else if (method != NVB197_SET_SCISSOR_ENABLE(0) && method != NVB197_BEGIN &&
                       method != NVB197_SET_VERTEX_ARRAY_START && method != NVB197_DRAW_VERTEX_ARRAY) {
                others.push_back(method);
            }
        }
    }

    void draw()
    {
        draws++;
        const uint32_t x0 = scissorH & 0xffff, x1 = scissorH >> 16, y0 = scissorV & 0xffff, y1 = scissorV >> 16;
        for (uint32_t y = footprint.y0; y < footprint.y1; y++)
            for (uint32_t x = footprint.x0; x < footprint.x1; x++)
                if (x >= x0 && x < x1 && y >= y0 && y < y1)
                    hits[size_t(y) * rot.planeWidth() + x]++;
    }
};

std::vector<uint32_t> banded(const Rotation &rot, const PixelBox &area, const PixelBox &box, uint32_t n,
                             uint32_t &rooms)
{
    std::vector<uint32_t> words(4096);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::emitBands(
        p, rot, area, box, n,
        [&] {
            rooms++;
        },
        [&](raw::Push &q) {
            q.m(NVB197_BEGIN, 0);
            q.inc(NVB197_SET_VERTEX_ARRAY_START, {0, 6});
            q.m(NVB197_END, 0);
        },
        [&] {
            p.m(NVB197_NO_OPERATION, 0);
        });
    words.resize(size_t(p.at - words.data()));
    return words;
}

TEST(EmitBands, ABandedDrawDrawsEachPixelOnceUnderItsScissor)
{
    const uint32_t w = 64, h = 40;
    const PixelBox area = {4, 3, 60, 37}, box = {10, 12, 50, 30};
    for (uint8_t turn = 0; turn <= 4; turn++) {
        const Rotation rot = turned(turn, w, h);
        uint32_t rooms = 0;
        Raster r;
        r.rot = rot;
        r.footprint = rot.rect({0, 0, w, h});
        r.run(banded(rot, area, box, 6, rooms));
        EXPECT_EQ(r.draws, 6u);
        EXPECT_EQ(r.cuts, 6u);
        EXPECT_EQ(rooms, 7u);
        EXPECT_TRUE(r.others.empty()) << "method " << (r.others.empty() ? 0 : r.others[0]);
        const Rect whole = rot.rect({area.x0, area.y0, area.x1, area.y1});
        EXPECT_EQ(r.scissorH, whole.x1 << 16 | whole.x0);
        EXPECT_EQ(r.scissorV, whole.y1 << 16 | whole.y0);
        for (uint32_t y = 0; y < rot.planeHeight(); y++)
            for (uint32_t x = 0; x < rot.planeWidth(); x++) {
                const bool inside = x >= whole.x0 && x < whole.x1 && y >= whole.y0 && y < whole.y1;
                EXPECT_EQ(r.hits[size_t(y) * rot.planeWidth() + x], inside ? 1 : 0)
                    << "turn " << int(turn) << " at " << x << "," << y;
            }
    }
}

}  // namespace
