// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC's colour stages (TegraVICColor.h): the gamut table against the chromaticities it comes from, what the request
// path takes and refuses, the configuration a taken change writes, and the model's output against Apple's
// (vic_color_reference.h).
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../TegraVICRequest.h"
#include "vic_color_reference.h"
#include "vic_model.h"

namespace {
using Matrix = std::array<std::array<double, 3>, 3>;

struct Primaries {
    uint32_t code;
    double xy[3][2];
};

constexpr Primaries kPrimaries[] = {
    {TEGRA_VIC_PRIMARIES_709, {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
    {TEGRA_VIC_PRIMARIES_P3_D65, {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}}},
    {TEGRA_VIC_PRIMARIES_2020, {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}},
};
constexpr double kD65[2] = {0.3127, 0.3290};
constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x20000000;

Matrix inverse(const Matrix &m)
{
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Matrix o;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            o[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / det;
        }
    return o;
}

Matrix product(const Matrix &a, const Matrix &b)
{
    Matrix o{};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                o[r][c] += a[r][k] * b[k][c];
    return o;
}

Matrix toXyz(uint32_t code)
{
    const Primaries *p = nullptr;
    for (const auto &q : kPrimaries)
        if (q.code == code)
            p = &q;
    EXPECT_NE(p, nullptr) << code;
    Matrix m;
    for (int i = 0; i < 3; i++) {
        const double x = p->xy[i][0], y = p->xy[i][1];
        m[0][i] = x / y;
        m[1][i] = 1;
        m[2][i] = (1 - x - y) / y;
    }
    const double white[3] = {kD65[0] / kD65[1], 1, (1 - kD65[0] - kD65[1]) / kD65[1]};
    const Matrix mi = inverse(m);
    for (int i = 0; i < 3; i++) {
        const double s = mi[i][0] * white[0] + mi[i][1] * white[1] + mi[i][2] * white[2];
        for (int r = 0; r < 3; r++)
            m[r][i] *= s;
    }
    return m;
}

TegraVICSurfaceDesc bgra(uint32_t width, uint32_t height, uint32_t transfer, uint32_t primaries)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    d.width = width;
    d.height = height;
    d.planeBytesPerRow[0] = (width * 4 + 255) & ~255u;
    d.planeHeight[0] = height;
    d.length = d.planeBytesPerRow[0] * height;
    d.transfer = transfer;
    d.primaries = primaries;
    return d;
}

struct Transfer {
    const char *why = nullptr;
    uint32_t kind = TEGRA_VIC_PLAN_OK;
    TegraVICPlan plan = {};
    TegraVICJob job = {};
};

Transfer transfer(const TegraVICSurfaceDesc &src, const TegraVICSurfaceDesc &dst, const TegraVICRequest &r = {})
{
    Transfer t;
    t.why = tegra_vic_plan(&r, &src, &dst, &t.plan, &t.kind);
    TegraVICImage si = {}, di = {};
    if (!t.why)
        t.why = tegra_vic_image_from(&src, kSrcIova, &si);
    if (!t.why)
        t.why = tegra_vic_image_from(&dst, kDstIova, &di);
    if (!t.why)
        t.why =
            tegra_vic_build(&si, &di, &t.plan.op[t.plan.count - 1], &kTegraVICTraits, 0x30000000, 0x30001000, &t.job);
    return t;
}
}  // namespace

TEST(TegraVICColor, EveryGamutIsItsPrimariesConversionAtTheWidestShift)
{
    for (const TegraVICGamut &g : kTegraVICGamuts) {
        const Matrix m = product(inverse(toXyz(g.to)), toXyz(g.from));
        double largest = 0;
        for (const auto &row : m)
            for (double v : row)
                largest = std::fmax(largest, std::fabs(v));
        uint32_t shift = 15;
        while (shift && largest * std::ldexp(1.0, 8 + shift) >= std::ldexp(1.0, 19))
            shift--;
        EXPECT_EQ(g.shift, shift) << g.from << " -> " << g.to;
        const int64_t one = int64_t(1) << (8 + shift);
        for (int r = 0; r < 3; r++) {
            int64_t row[3], sum = 0;
            for (int i = 0; i < 3; i++)
                sum += row[i] = std::llround(m[r][i] * double(one));
            row[r] += one - sum;
            for (int i = 0; i < 3; i++)
                EXPECT_EQ(g.c[r][i], row[i]) << g.from << " -> " << g.to << " [" << r << "][" << i << "]";
        }
    }
}

TEST(TegraVICColor, AGamutChangeBetweenSrgbSurfacesWritesItsThreeStages)
{
    const Transfer t = transfer(bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_709),
                                bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65));
    ASSERT_EQ(t.why, nullptr) << t.why;
    EXPECT_EQ(TEGRA_VIC_GET(t.job.config, TEGRA_VIC_SLOTS, SLOT_DEGAMMA), TEGRA_VIC_GAMMA_SRGB);
    EXPECT_EQ(TEGRA_VIC_GET(t.job.config, TEGRA_VIC_SLOTS, SLOT_CLIP_ENABLED), 1u);
    EXPECT_EQ(TEGRA_VIC_GET(t.job.config, TEGRA_VIC_OUTPUT, OUT_REGAMMA), TEGRA_VIC_GAMMA_SRGB);
    TegraVICMatrix m = {};
    ASSERT_TRUE(vic_model_matrix(t.job.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_GAMUT, &m));
    EXPECT_EQ(m.shift, kTegraVICGamuts[0].shift);
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++)
            EXPECT_EQ(m.c[r][i], kTegraVICGamuts[0].c[r][i]);
        EXPECT_EQ(m.c[r][3], 0);
    }

    const Transfer same = transfer(bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65),
                                   bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65));
    ASSERT_EQ(same.why, nullptr) << same.why;
    EXPECT_EQ(TEGRA_VIC_GET(same.job.config, TEGRA_VIC_SLOTS, SLOT_DEGAMMA), TEGRA_VIC_GAMMA_NONE);
    EXPECT_EQ(TEGRA_VIC_GET(same.job.config, TEGRA_VIC_OUTPUT, OUT_REGAMMA), TEGRA_VIC_GAMMA_NONE);
    EXPECT_FALSE(vic_model_matrix(same.job.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_GAMUT, &m));
}

TEST(TegraVICColor, ASideWithoutATagOrTheSameCurveConvertsNothing)
{
    const uint32_t cases[][4] = {
        {TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_709, 0, TEGRA_VIC_PRIMARIES_709},
        {TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_709, TEGRA_VIC_TF_SRGB, 2},
        {2, 2, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65},
        {TEGRA_VIC_TF_2020_10, TEGRA_VIC_PRIMARIES_2020, TEGRA_VIC_TF_709, TEGRA_VIC_PRIMARIES_2020},
        {TEGRA_VIC_TF_SRGB, 0, 0, TEGRA_VIC_PRIMARIES_709},
    };
    for (const auto &c : cases) {
        const Transfer t = transfer(bgra(64, 64, c[0], c[1]), bgra(64, 64, c[2], c[3]));
        EXPECT_EQ(t.why, nullptr) << c[0] << "/" << c[1] << " -> " << c[2] << "/" << c[3] << ": " << t.why;
        EXPECT_EQ(t.plan.op[0].gamut, nullptr);
        EXPECT_EQ(TEGRA_VIC_GET(t.job.config, TEGRA_VIC_OUTPUT, OUT_REGAMMA), TEGRA_VIC_GAMMA_NONE);
    }
}

TEST(TegraVICColor, ChangesVICDoesNotMatchAreRefusedAsUnsupported)
{
    const struct {
        uint32_t tags[4];
        bool ycc;
        const char *why;
    } cases[] = {
        {{TEGRA_VIC_TF_709, 1, TEGRA_VIC_TF_SRGB, 1}, false, "transfer function change (VIC's curves are not Apple's)"},
        {{TEGRA_VIC_TF_SRGB, 1, 8, 1}, false, "transfer function change (VIC's curves are not Apple's)"},
        {{TEGRA_VIC_TF_PQ, 9, TEGRA_VIC_TF_709, 9}, false, "PQ or HLG transfer change (no such curve on VIC)"},
        {{TEGRA_VIC_TF_SRGB, 1, TEGRA_VIC_TF_HLG, 1}, false, "PQ or HLG transfer change (no such curve on VIC)"},
        {{TEGRA_VIC_TF_709, 1, TEGRA_VIC_TF_709, 12}, false, "gamut change outside sRGB-encoded surfaces"},
        {{0, 1, TEGRA_VIC_TF_SRGB, 12}, false, "gamut change outside sRGB-encoded surfaces"},
        {{TEGRA_VIC_TF_SRGB, 1, 2, 12}, false, "gamut change outside sRGB-encoded surfaces"},
        {{TEGRA_VIC_TF_SRGB, 12, TEGRA_VIC_TF_SRGB, 1}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 9, TEGRA_VIC_TF_SRGB, 1}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 9, TEGRA_VIC_TF_SRGB, 12}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 12, TEGRA_VIC_TF_SRGB, 6}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 11, TEGRA_VIC_TF_SRGB, 12}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 1, TEGRA_VIC_TF_SRGB, 11}, false, "primaries change VIC does not take"},
        {{TEGRA_VIC_TF_SRGB, 12, TEGRA_VIC_TF_SRGB, 1}, true, "gamut change with a side that is not 8-bit RGB"},
    };

    for (const auto &c : cases) {
        TegraVICSurfaceDesc src = bgra(64, 64, c.tags[0], c.tags[1]);
        if (c.ycc) {
            src.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'f');
            src.planes = 2;
            src.planeBytesPerRow[0] = src.planeBytesPerRow[1] = 256;
            src.planeOffset[1] = 256 * 64;
            src.planeHeight[1] = 32;
            src.length = tegra_vic_surface_extent(&src);
        }
        const Transfer t = transfer(src, bgra(64, 64, c.tags[2], c.tags[3]));
        EXPECT_STREQ(t.why, c.why);
        EXPECT_EQ(t.kind, uint32_t(TEGRA_VIC_PLAN_UNSUPPORTED)) << c.why;
    }
}

TEST(TegraVICColor, AGamutChangeRunsOnlyWhereNothingIsFiltered)
{
    const TegraVICSurfaceDesc src = bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_709);
    const TegraVICSurfaceDesc dst = bgra(64, 64, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65);
    TegraVICRequest r = {};
    r.dst_w = r.dst_h = 32;
    EXPECT_STREQ(transfer(src, dst, r).why, "gamut change that scales or offsets by a fraction");
    r.src_x = 1u << 15;
    r.src_w = r.src_h = 32ull << 16;
    EXPECT_STREQ(transfer(src, dst, r).why, "gamut change that scales or offsets by a fraction");
    r.src_x = 8ull << 16;
    EXPECT_EQ(transfer(src, dst, r).why, nullptr);
    r = {};
    r.flags = TEGRA_VIC_ROT90;
    const Transfer turned = transfer(src, dst, r);
    EXPECT_EQ(turned.why, nullptr) << turned.why;
    EXPECT_EQ(TEGRA_VIC_GET(turned.job.config, TEGRA_VIC_OUTPUT, OUT_REGAMMA), TEGRA_VIC_GAMMA_SRGB);
}

// Core Animation's copies for a detached layer (IOMFBDisplay::detach_layer, both builds) tag the copy with the source's
// SurfaceColorPrimaries, which bucket every tag but P3_D65 and 2020 as BT.709 and write that bucket back as code 1, and
// copy the source's transfer function onto it (set_transfer_function(0) writes IEC_sRGB).
TEST(TegraVICColor, CoreAnimationsOwnCopiesChangeNoColour)
{
    const uint32_t transfers[] = {0, 2, TEGRA_VIC_TF_709, TEGRA_VIC_TF_SRGB, TEGRA_VIC_TF_2020_10, TEGRA_VIC_TF_PQ};
    for (uint32_t primaries = 0; primaries < 256; primaries++) {
        const uint32_t bucket = primaries == TEGRA_VIC_PRIMARIES_P3_D65 || primaries == TEGRA_VIC_PRIMARIES_2020
                                    ? primaries
                                    : TEGRA_VIC_PRIMARIES_709;
        for (uint32_t tf : transfers) {
            const uint32_t copied = tegra_vic_color_code(tf) ? tf : TEGRA_VIC_TF_SRGB;
            for (int rgb8 = 0; rgb8 < 2; rgb8++) {
                const TegraVICGamut *gamut = &kTegraVICGamuts[0];
                EXPECT_EQ(tegra_vic_color_plan(tf, primaries, copied, bucket, rgb8, &gamut), nullptr)
                    << primaries << " " << tf;
                EXPECT_EQ(gamut, nullptr) << primaries << " " << tf;
            }
        }
    }
    const Transfer smpteC = transfer(bgra(64, 64, TEGRA_VIC_TF_709, 6), bgra(64, 64, TEGRA_VIC_TF_709, 1));
    ASSERT_EQ(smpteC.why, nullptr) << smpteC.why;
    EXPECT_EQ(TEGRA_VIC_GET(smpteC.job.config, TEGRA_VIC_OUTPUT, OUT_REGAMMA), TEGRA_VIC_GAMMA_NONE);
}

// Apple's scaler passes SMPTE C and EBU 3213 through as BT.709, so into Display P3 they take 709's gamut.
TEST(TegraVICColor, SmpteCAndEbuAreTheScalers709)
{
    for (uint32_t primaries : {uint32_t(TEGRA_VIC_PRIMARIES_EBU), uint32_t(TEGRA_VIC_PRIMARIES_SMPTE_C)}) {
        const TegraVICGamut *gamut = nullptr;
        EXPECT_EQ(tegra_vic_color_plan(TEGRA_VIC_TF_SRGB, primaries, TEGRA_VIC_TF_SRGB, TEGRA_VIC_PRIMARIES_P3_D65, 1,
                                       &gamut),
                  nullptr);
        EXPECT_EQ(gamut, &kTegraVICGamuts[0]);
    }
}

TEST(TegraVICColor, TheModelWritesWithinOneCodeOfAppleForEveryTakenPair)
{
    ASSERT_EQ(sizeof(kVicColorReference) / sizeof(kVicColorReference[0]),
              sizeof(kTegraVICGamuts) / sizeof(kTegraVICGamuts[0]));
    for (const VicColorReference &ref : kVicColorReference) {
        const TegraVICSurfaceDesc src = bgra(64, 2, TEGRA_VIC_TF_SRGB, ref.from);
        const TegraVICSurfaceDesc dst = bgra(64, 2, TEGRA_VIC_TF_SRGB, ref.to);
        const Transfer t = transfer(src, dst);
        ASSERT_EQ(t.why, nullptr) << t.why;
        std::vector<uint8_t> in(src.length), out(dst.length);
        for (size_t i = 0; i < 125; i++) {
            const VicColorSample &s = ref.samples[i];
            const uint8_t px[4] = {s.in[2], s.in[1], s.in[0], 255};
            memcpy(&in[i * 4], px, 4);
        }
        VicModel model = {};
        model.region[0] = {kSrcIova, in.data(), in.size()};
        model.region[1] = {kDstIova, out.data(), out.size()};
        model.regions = 2;
        vic_model_run(&model, &t.job);
        ASSERT_EQ(model.faults, 0u);
        for (size_t i = 0; i < 125; i++) {
            const VicColorSample &s = ref.samples[i];
            for (int c = 0; c < 3; c++) {
                const int got = out[i * 4 + 2 - c];
                EXPECT_LE(std::abs(got - s.hw[c]), 1)
                    << ref.from << " -> " << ref.to << " sample " << i << " channel " << c;
                EXPECT_LE(std::abs(got - s.metal[c]), 1)
                    << ref.from << " -> " << ref.to << " sample " << i << " channel " << c;
            }
        }
    }
}

TEST(TegraVICColor, BulkAttachmentCodePointsBecomeTheSurfaceTags)
{
    EXPECT_EQ(switch_vic_matrix_code(0), uint32_t(SWITCH_VIC_MATRIX_UNTAGGED));
    EXPECT_EQ(switch_vic_matrix_code(2), uint32_t(SWITCH_VIC_MATRIX_UNTAGGED));
    EXPECT_EQ(switch_vic_matrix_code(1), uint32_t(SWITCH_VIC_MATRIX_709));
    EXPECT_EQ(switch_vic_matrix_code(5), uint32_t(SWITCH_VIC_MATRIX_601));
    EXPECT_EQ(switch_vic_matrix_code(6), uint32_t(SWITCH_VIC_MATRIX_601));
    EXPECT_EQ(switch_vic_matrix_code(9), uint32_t(SWITCH_VIC_MATRIX_2020));
    EXPECT_EQ(switch_vic_matrix_code(7), uint32_t(SWITCH_VIC_MATRIX_UNSUPPORTED));
    EXPECT_EQ(tegra_vic_color_code(2), 0u);
    EXPECT_EQ(tegra_vic_color_code(TEGRA_VIC_TF_SRGB), uint32_t(TEGRA_VIC_TF_SRGB));
    EXPECT_EQ(switch_surface_chroma_code(6), 6u);
    EXPECT_EQ(switch_surface_chroma_code(7), UINT32_MAX);
}
