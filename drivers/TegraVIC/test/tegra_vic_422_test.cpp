// SPDX-License-Identifier: BSD-2-Clause-Patent
// 4:2:2 biplanar ('422v', '422f') through TegraVIC: the request Core Animation's detached-layer copy sends on iPadOS 27
// (a 1920x1080 '420v' frame a quarter turn into a 300x534 '422v'), the configuration tegra_vic_build makes for it and
// for the other 4:2:2 pairs, the bounce geometry, and what vic_model.h writes for those jobs under the unit's traits.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../TegraVICBounce.h"
#include "../TegraVICConfig.h"
#include "../TegraVICRequest.h"
#include "TegraVICProbe.h"
#include "vic_model.h"

namespace {

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;
constexpr uint32_t k420v = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
constexpr uint32_t k422v = TEGRA_VIC_FOURCC('4', '2', '2', 'v');
constexpr uint32_t k422f = TEGRA_VIC_FOURCC('4', '2', '2', 'f');
constexpr uint32_t kBgra = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');

TegraVICSurfaceDesc surface(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = fourcc;
    d.width = w;
    d.height = h;
    const TegraVICFormat *f = tegra_vic_format(fourcc);
    d.planes = f->planes > 1 ? 2 : 0;
    for (uint32_t p = 0; p < f->planes; p++) {
        d.planeOffset[p] = p ? (d.planeBytesPerRow[0] * h + 0xfff) & ~0xfffull : 0;
        d.planeBytesPerRow[p] = pitch;
        d.planeWidth[p] = tegra_vic_plane_width(f, w, p);
        d.planeHeight[p] = tegra_vic_plane_rows(f, h, p);
    }
    d.length = (tegra_vic_surface_extent(&d) + 0x3fff) & ~0x3fffull;
    return d;
}

TegraVICRequest detachCopy(uint32_t transform, uint32_t site)
{
    TegraVICRequest r = {};
    r.src_id = 0x11;
    r.dst_id = 0x22;
    r.cb_fn = 0x18966cbb0;
    r.flags = transform | 0x2000 | 0x1000 | (uint64_t)site << 15;
    return r;
}

struct Built {
    const char *why;
    TegraVICJob job;
};

Built build(const TegraVICSurfaceDesc &s, const TegraVICSurfaceDesc &d, const TegraVICRequest &r)
{
    Built b = {};
    TegraVICPlan plan;
    uint32_t kind = 0;
    b.why = tegra_vic_plan(&r, &s, &d, &plan, &kind);
    TegraVICImage src = {}, dst = {};
    if (!b.why)
        b.why = tegra_vic_image_from(&d, kDstIova, &dst);
    if (!b.why)
        b.why = tegra_vic_image_from(&s, kSrcIova, &src);
    dst.chromaSite = TEGRA_VIC_REQ_CHROMA_SITE(r.flags);
    for (uint32_t i = 0; !b.why && i < plan.count; i++)
        b.why = tegra_vic_build(plan.op[i].fill ? nullptr : &src, &dst, &plan.op[i], &kTegraVICTraits, kConfig, kFilter,
                                &b.job);
    return b;
}

uint64_t out(const TegraVICJob &j, uint32_t bit, uint32_t width)
{
    return tegra_vic_get(j.config, TEGRA_VIC_OUTPUT_SURFACE, bit, width);
}

#define OUT_FIELD(j, name) out((j), name##_BIT, name##_WIDTH)

// The refused request: 1920x1080 '420v' (rows on 256 bytes, as everything sized for this display) a quarter turn and
// 3.6x down into a 300x534 '422v'. The output is described before the transpose: 534x300 of N422R, luma 534 rows of
// 512, chroma 534 rows of 256 pairs, the siting exchanged.
TEST(Vic422, TheDetachedLayerCopyBuilds)
{
    const TegraVICSurfaceDesc src = surface(k420v, 1920, 1080, 2048);
    const TegraVICSurfaceDesc dst = surface(k422v, 300, 534, 512);
    for (uint32_t transform : {4u, 7u}) {
        const Built b = build(src, dst, detachCopy(transform, 2));
        ASSERT_EQ(b.why, nullptr) << b.why;
        EXPECT_EQ(OUT_FIELD(b.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8V8_N422R));
        EXPECT_EQ(OUT_FIELD(b.job, SURF_WIDTH), 533u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_HEIGHT), 299u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_LUMA_WIDTH), 533u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_LUMA_HEIGHT), 511u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_WIDTH), 533u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_HEIGHT), 255u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_LOC_HORIZ), 0u);
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_LOC_VERT), 1u) << "Center siting across memory is down before it";
        EXPECT_EQ(TEGRA_VIC_GET(b.job.config, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE), 1u);
    }
    const Built flat = build(src, surface(k422v, 534, 300, 768), detachCopy(0, 2));
    ASSERT_EQ(flat.why, nullptr) << flat.why;
    EXPECT_EQ(OUT_FIELD(flat.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8V8_N422));
    EXPECT_EQ(OUT_FIELD(flat.job, SURF_CHROMA_WIDTH), 383u);
    EXPECT_EQ(OUT_FIELD(flat.job, SURF_CHROMA_HEIGHT), 299u);
    EXPECT_EQ(OUT_FIELD(flat.job, SURF_CHROMA_LOC_HORIZ), 1u);
    EXPECT_EQ(OUT_FIELD(flat.job, SURF_CHROMA_LOC_VERT), 0u);
}

// Transposed 4:2:0 output keeps the siting it ran with on the unit; only 4:2:2's is exchanged.
TEST(Vic422, Transposed420KeepsItsSiting)
{
    const TegraVICSurfaceDesc src = surface(kBgra, 720, 1280, 2880 + 192);
    const TegraVICSurfaceDesc dst = surface(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 1280, 720, 1280);
    for (uint32_t site : {1u, 2u, 5u}) {
        const Built b = build(src, dst, detachCopy(4, site));
        ASSERT_EQ(b.why, nullptr) << b.why;
        EXPECT_EQ(OUT_FIELD(b.job, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8V8_N420));
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_LOC_HORIZ), tegra_vic_chroma_horiz(site)) << "site " << site;
        EXPECT_EQ(OUT_FIELD(b.job, SURF_CHROMA_LOC_VERT), tegra_vic_chroma_vert(site)) << "site " << site;
    }
}

// Both surfaces as sized for this display go straight to VIC; with CoreVideo's 64-byte rows either side is bounced
// through scratch whose chroma plane keeps every row.
TEST(Vic422, DirectOrBounced)
{
    const TegraVICSurfaceDesc frame = surface(k420v, 1920, 1080, 2048);
    const TegraVICSurfaceDesc copy = surface(k422v, 300, 534, 512);
    EXPECT_EQ(tegra_vic_bounce_needed(&frame, 256), 1);
    EXPECT_EQ(tegra_vic_bounce_needed(&copy, 256), 1);
    const TegraVICSurfaceDesc tight = surface(k422v, 300, 534, 320);
    ASSERT_EQ(tegra_vic_bounce_needed(&tight, 256), 0);
    TegraVICSurfaceDesc scratch;
    tegra_vic_bounce_desc(&tight, 256, &scratch);
    EXPECT_EQ(scratch.planeBytesPerRow[0], 512u);
    EXPECT_EQ(scratch.planeBytesPerRow[1], 512u);
    EXPECT_EQ(scratch.planeHeight[1], 534u);
    EXPECT_EQ(tegra_vic_bounce_row_bytes(tegra_vic_format(k422v), 300, 1), 300u);
    EXPECT_GE(scratch.length, scratch.planeOffset[1] + 512ull * 534);
}

TEST(Vic422, ShapesAndRefusals)
{
    const TegraVICSurfaceDesc src = surface(k420v, 1920, 1080, 2048);
    EXPECT_EQ(build(src, surface(k422v, 300, 533, 512), detachCopy(4, 1)).why, nullptr) << "odd height";
    EXPECT_STREQ(build(src, surface(k422v, 301, 534, 512), detachCopy(4, 1)).why, "4:2:2 surface of odd width");
    TegraVICRequest r = detachCopy(0, 1);
    r.dst_x = 1;
    r.dst_w = 200;
    r.dst_h = 100;
    EXPECT_STREQ(build(src, surface(k422v, 534, 300, 768), r).why, "4:2:2 destination rectangle on odd columns");
    r.dst_x = 2;
    r.dst_y = 1;
    EXPECT_EQ(build(src, surface(k422v, 534, 300, 768), r).why, nullptr) << "odd rows are whole chroma rows";

    // A 4:2:2 source: the slot's chroma plane has every row.
    const Built in = build(surface(k422f, 640, 360, 768), surface(kBgra, 640, 360, 2560), detachCopy(0, 1));
    ASSERT_EQ(in.why, nullptr) << in.why;
    const uint32_t slot = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    EXPECT_EQ(TEGRA_VIC_GET(in.job.config, slot, SURF_FORMAT), uint64_t(TEGRA_VIC_T_Y8___U8V8_N422));
    EXPECT_EQ(TEGRA_VIC_GET(in.job.config, slot, SURF_CHROMA_HEIGHT), 359u);
    EXPECT_EQ(TEGRA_VIC_GET(in.job.config, slot, SURF_CHROMA_LOC_VERT), 0u);

    // A WriteOnlyMode fill of a 4:2:2 rectangle widens it across only.
    TegraVICRequest fill = detachCopy(0, 1);
    fill.flags |= TEGRA_VIC_REQ_WRITE_ONLY;
    fill.dst_x = 3;
    fill.dst_y = 3;
    fill.dst_w = 4;
    fill.dst_h = 3;
    TegraVICPlan plan;
    uint32_t kind = 0;
    const TegraVICSurfaceDesc d = surface(k422v, 64, 32, 256);
    ASSERT_EQ(tegra_vic_plan(&fill, &d, &d, &plan, &kind), nullptr);
    EXPECT_EQ(plan.op[0].dstX, 2u);
    EXPECT_EQ(plan.op[0].dstW, 6u);
    EXPECT_EQ(plan.op[0].dstY, 3u);
    EXPECT_EQ(plan.op[0].dstH, 3u);
}

// --- On the layout model (VIC_MODEL_FLIP_SURFACE: the unit) -------------------------------------------------------
struct Bench {
    std::vector<uint8_t> src, dst;
    VicModel model = {};

    Bench(size_t srcBytes, size_t dstBytes) : src(srcBytes), dst(dstBytes, TEGRA_VIC_POISON_BYTE)
    {
        model.region[0] = {kSrcIova, src.data(), src.size()};
        model.region[1] = {kDstIova, dst.data(), dst.size()};
        model.regions = 2;
        model.flips = VIC_MODEL_FLIP_SURFACE;
    }
};

TegraVICImage planes(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch, uint64_t iova)
{
    const TegraVICSurfaceDesc d = surface(fourcc, w, h, pitch);
    TegraVICImage im = {};
    EXPECT_EQ(tegra_vic_image_from(&d, iova, &im), nullptr);
    im.standard = TEGRA_VIC_YCC_709;
    return im;
}

uint64_t chromaOffset(uint32_t pitch, uint32_t h)
{
    return (uint64_t(pitch) * h + 0xfff) & ~0xfffull;
}

// A BGRA card (every pixel its own colour) turned into '422v' by each transposed transform: every luma sample and every
// CbCr pair (the even column's) lands where the turned card has its colour, and nothing past the planes is written.
TEST(Vic422Model, QuarterTurnsOfBgraInto422v)
{
    constexpr uint32_t SW = TEGRA_VIC_CARD_W, SH = TEGRA_VIC_CARD_H, DW = SH, DH = SW, SP = 256, DP = 256;
    TegraVICMatrix m;
    tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, false, &kTegraVICTraits.out, &m);
    const auto ycc = [&](uint32_t argb, uint32_t e[3]) {
        const uint32_t in10[3] = {((argb >> 16) & 0xff) << 2, ((argb >> 8) & 0xff) << 2, (argb & 0xff) << 2};
        uint32_t o[3];
        tegra_vic_matrix_apply(&m, in10, o);
        e[0] = o[0] >> 2;
        e[1] = o[kTegraVICTraits.out.cbChannel] >> 2;
        e[2] = o[kTegraVICTraits.out.crChannel] >> 2;
    };
    const uint64_t cOff = chromaOffset(DP, DH);
    for (uint32_t tr : {4u, 5u, 6u, 7u}) {
        Bench b(size_t(SP) * SH, cOff + size_t(DP) * DH + 4096);
        for (uint32_t y = 0; y < SH; y++)
            for (uint32_t x = 0; x < SW; x++) {
                const uint32_t w = tegra_vic_card_pixel(x, y);
                memcpy(b.src.data() + size_t(y) * SP + x * 4, &w, 4);
            }
        const TegraVICImage src = planes(kBgra, SW, SH, SP, kSrcIova);
        const TegraVICImage dst = planes(k422v, DW, DH, DP, kDstIova);
        TegraVICOp op = tegra_vic_probe_op(SW, SH, DW, DH, tr);
        TegraVICJob job;
        const char *why = tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, kConfig, kFilter, &job);
        ASSERT_EQ(why, nullptr) << why;
        vic_model_run(&b.model, &job);
        EXPECT_EQ(b.model.writes, uint64_t(DW) * DH + uint64_t(DW / 2) * DH) << "transform " << tr;
        EXPECT_EQ(b.model.clipped + b.model.faults, 0u) << "transform " << tr;
        std::vector<uint32_t> want(size_t(DW) * DH);
        for (uint32_t y = 0; y < SH; y++)
            for (uint32_t x = 0; x < SW; x++) {
                uint32_t u, v;
                tegra_vic_transform_point(tr, SW, SH, x, y, &u, &v);
                want[size_t(v) * DW + u] = tegra_vic_card_pixel(x, y);
            }
        uint32_t lumaOff = 0, chromaOff = 0;
        for (uint32_t v = 0; v < DH; v++)
            for (uint32_t u = 0; u < DW; u++) {
                uint32_t e[3];
                ycc(want[size_t(v) * DW + u], e);
                lumaOff += b.dst[size_t(v) * DP + u] != e[0];
                if (u & 1)
                    continue;
                const uint8_t *cq = b.dst.data() + cOff + size_t(v) * DP + u;
                chromaOff += cq[0] != e[1] || cq[1] != e[2];
            }
        EXPECT_EQ(lumaOff, 0u) << "transform " << tr;
        EXPECT_EQ(chromaOff, 0u) << "transform " << tr;
        for (size_t i = cOff + size_t(DP) * DH; i < b.dst.size(); i++)
            ASSERT_EQ(b.dst[i], TEGRA_VIC_POISON_BYTE) << "past the planes, transform " << tr;
    }
}

// The refused shape on the model: one colour in, so the 4:2:0 to 4:2:2 pass-through must give exactly it back on
// every luma sample and CbCr pair of 300x534, and nothing in the rows' padding or past the planes.
TEST(Vic422Model, TheDetachedLayerCopyCoversItsPlanes)
{
    constexpr uint32_t SW = 1920, SH = 1080, SP = 2048, DW = 300, DH = 534, DP = 512;
    constexpr uint8_t Y = 81, CB = 90, CR = 240;
    const uint64_t sOff = chromaOffset(SP, SH), cOff = chromaOffset(DP, DH);
    for (uint32_t tr : {4u, 7u}) {
        Bench b(sOff + size_t(SP) * SH / 2, cOff + size_t(DP) * DH + 4096);
        memset(b.src.data(), Y, sOff);
        for (size_t i = sOff; i < b.src.size(); i += 2) {
            b.src[i] = CB;
            b.src[i + 1] = CR;
        }
        const TegraVICImage src = planes(k420v, SW, SH, SP, kSrcIova);
        const TegraVICImage dst = planes(k422v, DW, DH, DP, kDstIova);
        TegraVICOp op = tegra_vic_probe_op(SW, SH, DW, DH, tr);
        op.nearest = 0;
        TegraVICJob job;
        const char *why = tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, kConfig, kFilter, &job);
        ASSERT_EQ(why, nullptr) << why;
        vic_model_run(&b.model, &job);
        EXPECT_EQ(b.model.writes, uint64_t(DW) * DH + uint64_t(DW / 2) * DH);
        EXPECT_EQ(b.model.clipped + b.model.faults, 0u);
        uint32_t off = 0, padding = 0;
        for (uint32_t v = 0; v < DH; v++)
            for (uint32_t u = 0; u < DP; u++) {
                const uint8_t l = b.dst[size_t(v) * DP + u], c = b.dst[cOff + size_t(v) * DP + u];
                if (u < DW) {
                    off += l != Y || c != (u & 1 ? CR : CB);
                } else {
                    padding += l != TEGRA_VIC_POISON_BYTE || c != TEGRA_VIC_POISON_BYTE;
                }
            }
        EXPECT_EQ(off, 0u) << "transform " << tr;
        EXPECT_EQ(padding, 0u) << "transform " << tr;
    }
}

// Untransposed: a '420v' copied 1:1 into '422v' repeats each chroma row; a '422v' read into BGRA takes each row's own.
TEST(Vic422Model, ChromaRowsEachWay)
{
    constexpr uint32_t W = 64, H = 32, P = 256;
    const uint64_t cOff = chromaOffset(P, H);
    {
        Bench b(cOff + size_t(P) * H / 2, cOff + size_t(P) * H);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
                b.src[size_t(y) * P + x] = uint8_t(16 + x + y);
        for (uint32_t r = 0; r < H / 2; r++)
            for (uint32_t x = 0; x < W; x++)
                b.src[cOff + size_t(r) * P + x] = uint8_t(100 + 3 * r + (x & 1) * 50);
        const TegraVICImage src = planes(k420v, W, H, P, kSrcIova);
        const TegraVICImage dst = planes(k422v, W, H, P, kDstIova);
        const TegraVICOp op = tegra_vic_probe_op(W, H, W, H, 0);
        TegraVICJob job;
        ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, kConfig, kFilter, &job), nullptr);
        vic_model_run(&b.model, &job);
        uint32_t off = 0;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                off += b.dst[size_t(y) * P + x] != b.src[size_t(y) * P + x];
                off += b.dst[cOff + size_t(y) * P + x] != b.src[cOff + size_t(y / 2) * P + x];
            }
        EXPECT_EQ(off, 0u);
    }
    {
        Bench b(cOff + size_t(P) * H, size_t(P) * 4 * H);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                b.src[size_t(y) * P + x] = 128;
                b.src[cOff + size_t(y) * P + x] = uint8_t(x & 1 ? 128 : 40 + 5 * y);
            }
        const TegraVICImage src = planes(k422v, W, H, P, kSrcIova);
        const TegraVICImage dst = planes(kBgra, W, H, P * 4, kDstIova);
        const TegraVICOp op = tegra_vic_probe_op(W, H, W, H, 0);
        TegraVICJob job;
        ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, kConfig, kFilter, &job), nullptr);
        vic_model_run(&b.model, &job);
        TegraVICMatrix m;
        tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_709, false, &kTegraVICTraits.in, &m);
        uint32_t off = 0;
        for (uint32_t y = 0; y < H; y++) {
            uint32_t in10[3], rgb[3];
            in10[0] = vic_model_widen(128);
            in10[kTegraVICTraits.in.cbChannel] = vic_model_widen(40 + 5 * y);
            in10[kTegraVICTraits.in.crChannel] = vic_model_widen(128);
            tegra_vic_matrix_apply(&m, in10, rgb);
            const uint32_t want = 0xff000000u | (rgb[0] >> 2) << 16 | (rgb[1] >> 2) << 8 | (rgb[2] >> 2);
            for (uint32_t x = 0; x < W; x++) {
                uint32_t got;
                memcpy(&got, b.dst.data() + size_t(y) * P * 4 + x * 4, 4);
                off += got != want;
            }
        }
        EXPECT_EQ(off, 0u);
    }
}

}  // namespace
