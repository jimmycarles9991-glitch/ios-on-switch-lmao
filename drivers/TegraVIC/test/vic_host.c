// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host checks of TegraVIC's job generation, request translation and checker (test:tegra_vic_test runs this):
//   layout     every ConfigStruct field TegraVICConfig.h packs lands exactly where libdrm's vic40.h (NVIDIA, MIT)
//              bitfields put it, and the structure is 0x610 bytes
//   colour     the generated matrices, through the engine's arithmetic (the law TegraVICConfig.h states), against the
//              float BT.601/BT.709 conversions in both ranges, over the whole RGB cube in steps; and round trips
//   jobs       the methods and configuration for the Screen Recording request and others, against expectations
//   requests   IOSurfaceAccelerator transform requests (0x1b0 bytes, as IOSurfaceAccelerator.framework builds them) as
//              Core Animation sends them, translated; and every kind of refused request
//   checker    each refusal of tegra_vic_build/tegra_vic_check_image
// Prints "PASS vic" and the counts; any failure prints its case and exits 1.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../TegraVICConfig.h"
#include "../TegraVICRequest.h"
#include "libdrm/vic40.h"

static int failures, checks;
#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

// --- layout ---------------------------------------------------------------------------------------------------------
_Static_assert(sizeof(ConfigStruct) == TEGRA_VIC_CONFIG_BYTES, "ConfigStruct is 0x610 bytes");
_Static_assert(sizeof(SlotStruct) == TEGRA_VIC_SLOT_BYTES, "SlotStruct is 0xb0 bytes");

// Sets a field both ways (libdrm's bitfield through `assign`, ours through tegra_vic_put) with an all-ones value and a
// pattern, and compares the whole structure.
#define FIELD(part, member, base, name)                                                                               \
    do {                                                                                                              \
        for (int pass = 0; pass < 2; pass++) {                                                                        \
            ConfigStruct ref;                                                                                         \
            uint8_t ours[TEGRA_VIC_CONFIG_BYTES];                                                                     \
            memset(&ref, 0, sizeof(ref));                                                                             \
            memset(ours, 0, sizeof(ours));                                                                            \
            const uint64_t v = pass ? 0x5555555555555555ull : ~0ull;                                                  \
            ref.part.member = v;                                                                                      \
            tegra_vic_put(ours, (base), name##_BIT, name##_WIDTH, v & ((1ull << name##_WIDTH) - 1));                  \
            CHECK(!memcmp(&ref, ours, sizeof(ours)), "field %s (%s.%s) differs from vic40.h", #name, #part, #member); \
        }                                                                                                             \
    } while (0)

static void layout(void)
{
    FIELD(pipeConfig, DownsampleHoriz, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_HORIZ);
    FIELD(pipeConfig, DownsampleVert, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_VERT);
    FIELD(outputConfig, AlphaFillMode, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE);
    FIELD(outputConfig, AlphaFillSlot, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_SLOT);
    FIELD(outputConfig, BackgroundAlpha, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA);
    FIELD(outputConfig, BackgroundR, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R);
    FIELD(outputConfig, BackgroundG, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G);
    FIELD(outputConfig, BackgroundB, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B);
    FIELD(outputConfig, OutputFlipX, TEGRA_VIC_OUTPUT, OUT_FLIP_X);
    FIELD(outputConfig, OutputFlipY, TEGRA_VIC_OUTPUT, OUT_FLIP_Y);
    FIELD(outputConfig, OutputTranspose, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE);
    FIELD(outputConfig, TargetRectLeft, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT);
    FIELD(outputConfig, TargetRectRight, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT);
    FIELD(outputConfig, TargetRectTop, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP);
    FIELD(outputConfig, TargetRectBottom, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM);
    FIELD(outputSurfaceConfig, OutPixelFormat, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT);
    FIELD(outputSurfaceConfig, OutChromaLocHoriz, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ);
    FIELD(outputSurfaceConfig, OutChromaLocVert, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT);
    FIELD(outputSurfaceConfig, OutBlkKind, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND);
    FIELD(outputSurfaceConfig, OutBlkHeight, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_HEIGHT);
    FIELD(outputSurfaceConfig, OutSurfaceWidth, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH);
    FIELD(outputSurfaceConfig, OutSurfaceHeight, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT);
    FIELD(outputSurfaceConfig, OutLumaWidth, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH);
    FIELD(outputSurfaceConfig, OutLumaHeight, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT);
    FIELD(outputSurfaceConfig, OutChromaWidth, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH);
    FIELD(outputSurfaceConfig, OutChromaHeight, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT);
    FIELD(outColorMatrixStruct, matrix_coeff00, TEGRA_VIC_OUT_MATRIX, MATRIX_C00);
    FIELD(outColorMatrixStruct, matrix_coeff10, TEGRA_VIC_OUT_MATRIX, MATRIX_C10);
    FIELD(outColorMatrixStruct, matrix_coeff20, TEGRA_VIC_OUT_MATRIX, MATRIX_C20);
    FIELD(outColorMatrixStruct, matrix_r_shift, TEGRA_VIC_OUT_MATRIX, MATRIX_SHIFT);
    FIELD(outColorMatrixStruct, matrix_coeff01, TEGRA_VIC_OUT_MATRIX, MATRIX_C01);
    FIELD(outColorMatrixStruct, matrix_coeff11, TEGRA_VIC_OUT_MATRIX, MATRIX_C11);
    FIELD(outColorMatrixStruct, matrix_coeff21, TEGRA_VIC_OUT_MATRIX, MATRIX_C21);
    FIELD(outColorMatrixStruct, matrix_enable, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE);
    FIELD(outColorMatrixStruct, matrix_coeff02, TEGRA_VIC_OUT_MATRIX, MATRIX_C02);
    FIELD(outColorMatrixStruct, matrix_coeff12, TEGRA_VIC_OUT_MATRIX, MATRIX_C12);
    FIELD(outColorMatrixStruct, matrix_coeff22, TEGRA_VIC_OUT_MATRIX, MATRIX_C22);
    FIELD(outColorMatrixStruct, matrix_coeff03, TEGRA_VIC_OUT_MATRIX, MATRIX_C03);
    FIELD(outColorMatrixStruct, matrix_coeff13, TEGRA_VIC_OUT_MATRIX, MATRIX_C13);
    FIELD(outColorMatrixStruct, matrix_coeff23, TEGRA_VIC_OUT_MATRIX, MATRIX_C23);
    const uint32_t s0 = TEGRA_VIC_SLOTS, s3 = TEGRA_VIC_SLOTS + 3 * TEGRA_VIC_SLOT_BYTES;
    FIELD(slotStruct[0].slotConfig, SlotEnable, s0, SLOT_ENABLE);
    FIELD(slotStruct[0].slotConfig, CurrentFieldEnable, s0, SLOT_CURRENT_FIELD);
    FIELD(slotStruct[0].slotConfig, FrameFormat, s0, SLOT_FRAME_FORMAT);
    FIELD(slotStruct[0].slotConfig, FilterLengthY, s0, SLOT_FILTER_Y);
    FIELD(slotStruct[0].slotConfig, FilterLengthX, s0, SLOT_FILTER_X);
    FIELD(slotStruct[0].slotConfig, SoftClampLow, s0, SLOT_SOFT_CLAMP_LOW);
    FIELD(slotStruct[0].slotConfig, SoftClampHigh, s0, SLOT_SOFT_CLAMP_HIGH);
    FIELD(slotStruct[0].slotConfig, PlanarAlpha, s0, SLOT_PLANAR_ALPHA);
    FIELD(slotStruct[0].slotConfig, ConstantAlpha, s0, SLOT_CONSTANT_ALPHA);
    FIELD(slotStruct[0].slotConfig, SourceRectLeft, s0, SLOT_SOURCE_LEFT);
    FIELD(slotStruct[0].slotConfig, SourceRectRight, s0, SLOT_SOURCE_RIGHT);
    FIELD(slotStruct[0].slotConfig, SourceRectTop, s0, SLOT_SOURCE_TOP);
    FIELD(slotStruct[0].slotConfig, SourceRectBottom, s0, SLOT_SOURCE_BOTTOM);
    FIELD(slotStruct[0].slotConfig, DestRectLeft, s0, SLOT_DEST_LEFT);
    FIELD(slotStruct[0].slotConfig, DestRectRight, s0, SLOT_DEST_RIGHT);
    FIELD(slotStruct[0].slotConfig, DestRectTop, s0, SLOT_DEST_TOP);
    FIELD(slotStruct[0].slotConfig, DestRectBottom, s0, SLOT_DEST_BOTTOM);
    FIELD(slotStruct[3].slotConfig, DestRectBottom, s3, SLOT_DEST_BOTTOM);
    const uint32_t f0 = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    FIELD(slotStruct[0].slotSurfaceConfig, SlotPixelFormat, f0, SURF_FORMAT);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotChromaLocHoriz, f0, SURF_CHROMA_LOC_HORIZ);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotChromaLocVert, f0, SURF_CHROMA_LOC_VERT);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotBlkKind, f0, SURF_BLK_KIND);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotBlkHeight, f0, SURF_BLK_HEIGHT);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotCacheWidth, f0, SURF_CACHE_WIDTH);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotSurfaceWidth, f0, SURF_WIDTH);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotSurfaceHeight, f0, SURF_HEIGHT);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotLumaWidth, f0, SURF_LUMA_WIDTH);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotLumaHeight, f0, SURF_LUMA_HEIGHT);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotChromaWidth, f0, SURF_CHROMA_WIDTH);
    FIELD(slotStruct[0].slotSurfaceConfig, SlotChromaHeight, f0, SURF_CHROMA_HEIGHT);
    const uint32_t m0 = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX;
    FIELD(slotStruct[0].colorMatrixStruct, matrix_coeff00, m0, MATRIX_C00);
    FIELD(slotStruct[0].colorMatrixStruct, matrix_coeff23, m0, MATRIX_C23);
    FIELD(slotStruct[0].colorMatrixStruct, matrix_enable, m0, MATRIX_ENABLE);
    FIELD(slotStruct[0].colorMatrixStruct, matrix_r_shift, m0, MATRIX_SHIFT);
}

// --- colour ---------------------------------------------------------------------------------------------------------
static const TegraVICChroma kMeasured = {2, 1};  // Cb in channel 2, Cr in channel 1 (switch-frame-tap, T210B01)

static void idealYcc(int standard, int full, double r, double g, double b, double out[3])
{
    const double kr = standard == TEGRA_VIC_YCC_709 ? 0.2126 : 0.299,
                 kb = standard == TEGRA_VIC_YCC_709 ? 0.0722 : 0.114;
    const double kg = 1 - kr - kb;
    const double y = (kr * r + kg * g + kb * b) / 255, cb = (b / 255 - y) / (2 * (1 - kb)),
                 cr = (r / 255 - y) / (2 * (1 - kr));
    out[0] = full ? 255 * y : 16 + 219 * y;
    out[1] = 128 + (full ? 255 : 224) * cb;
    out[2] = 128 + (full ? 255 : 224) * cr;
}

static double clamp255(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void colour(void)
{
    for (int standard = 0; standard < 2; standard++)
        for (int full = 0; full < 2; full++) {
            TegraVICMatrix toYcc, toRgb;
            tegra_vic_matrix_rgb_to_ycc((uint32_t)standard, full, &kMeasured, &toYcc);
            tegra_vic_matrix_ycc_to_rgb((uint32_t)standard, full, &kMeasured, &toRgb);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++)
                    CHECK(toYcc.c[i][j] > -(1 << 19) && toYcc.c[i][j] < (1 << 19) && toRgb.c[i][j] > -(1 << 19) &&
                              toRgb.c[i][j] < (1 << 19),
                          "coefficient outside 20 bits");
            double worst = 0, worstBack = 0;
            for (int r = 0; r < 256; r += 5)
                for (int g = 0; g < 256; g += 5)
                    for (int b = 0; b < 256; b += 5) {
                        const uint32_t in10[3] = {(uint32_t)r << 2, (uint32_t)g << 2, (uint32_t)b << 2};
                        uint32_t out10[3];
                        tegra_vic_matrix_apply(&toYcc, in10, out10);
                        double ideal[3];
                        idealYcc(standard, full, r, g, b, ideal);
                        const uint32_t got[3] = {out10[0] >> 2, out10[kMeasured.cbChannel] >> 2,
                                                 out10[kMeasured.crChannel] >> 2};
                        for (int c = 0; c < 3; c++) {
                            const double d = fabs(got[c] - clamp255(ideal[c]));
                            if (d > worst)
                                worst = d;
                        }
                        // Round trip through the engine's 8-bit storage.
                        uint32_t back10[3], mid[3] = {(out10[0] >> 2) << 2, (out10[1] >> 2) << 2, (out10[2] >> 2) << 2};
                        tegra_vic_matrix_apply(&toRgb, mid, back10);
                        const int rgb[3] = {r, g, b};
                        for (int c = 0; c < 3; c++) {
                            const double d = fabs((double)(back10[c] >> 2) - rgb[c]);
                            if (d > worstBack)
                                worstBack = d;
                        }
                    }
            // One 8-bit step against the float conversion; a round trip (two quantisations, 4:2:0 storage aside)
            // within what 8-bit Y'CbCr can hold (video range loses ~1.2 steps of chroma precision).
            CHECK(worst <= 1.0, "BT.%s %s RGB -> Y'CbCr off by %.2f steps", standard ? "709" : "601",
                  full ? "full" : "video", worst);
            CHECK(worstBack <= (full ? 3.0 : 4.0), "BT.%s %s round trip off by %.2f steps", standard ? "709" : "601",
                  full ? "full" : "video", worstBack);
        }
    // Inverse on the code values themselves: Y'CbCr grid -> RGB against the float inverse.
    for (int standard = 0; standard < 2; standard++)
        for (int full = 0; full < 2; full++) {
            TegraVICMatrix toRgb;
            tegra_vic_matrix_ycc_to_rgb((uint32_t)standard, full, &kMeasured, &toRgb);
            const double kr = standard ? 0.2126 : 0.299, kb = standard ? 0.0722 : 0.114, kg = 1 - kr - kb;
            double worst = 0;
            for (int y = full ? 0 : 16; y <= (full ? 255 : 235); y += 7)
                for (int cb = 16; cb <= 240; cb += 8)
                    for (int cr = 16; cr <= 240; cr += 8) {
                        uint32_t in10[3];
                        in10[0] = (uint32_t)y << 2;
                        in10[kMeasured.cbChannel] = (uint32_t)cb << 2;
                        in10[kMeasured.crChannel] = (uint32_t)cr << 2;
                        uint32_t out10[3];
                        tegra_vic_matrix_apply(&toRgb, in10, out10);
                        const double yy = full ? y / 255.0 : (y - 16) / 219.0;
                        const double pb = (cb - 128) / (full ? 255.0 : 224.0), pr = (cr - 128) / (full ? 255.0 : 224.0);
                        const double R = yy + 2 * (1 - kr) * pr, B = yy + 2 * (1 - kb) * pb;
                        const double G = (yy - kr * R - kb * B) / kg;
                        const double ideal[3] = {clamp255(255 * R), clamp255(255 * G), clamp255(255 * B)};
                        for (int c = 0; c < 3; c++) {
                            const double d = fabs((double)(out10[c] >> 2) - ideal[c]);
                            if (d > worst)
                                worst = d;
                        }
                    }
            CHECK(worst <= 1.0, "BT.%s %s Y'CbCr -> RGB off by %.2f steps", standard ? "709" : "601",
                  full ? "full" : "video", worst);
        }
}

// The self-test's integer reference for reading 4:2:0 against the float formula, over the whole code space.
static void idealReference(void)
{
    double worst = 0;
    for (uint32_t y = 0; y < 256; y++)
        for (uint32_t cb = 0; cb < 256; cb += 3)
            for (uint32_t cr = 0; cr < 256; cr += 3) {
                const uint32_t ycc[3] = {y, cb, cr};
                uint32_t rgb[3];
                tegra_vic_ideal_rgb709_full(ycc, rgb);
                const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb, pb = cb - 128.0, pr = cr - 128.0;
                const double R = y + 2 * (1 - kr) * pr, B = y + 2 * (1 - kb) * pb, G = (y - kr * R - kb * B) / kg;
                const double ideal[3] = {clamp255(R), clamp255(G), clamp255(B)};
                for (int c = 0; c < 3; c++)
                    if (fabs(rgb[c] - ideal[c]) > worst)
                        worst = fabs(rgb[c] - ideal[c]);
            }
    CHECK(worst <= 0.51, "integer BT.709 reference off the float one by %.3f", worst);
}

// --- jobs and checker ------------------------------------------------------------------------------------------------
static TegraVICTraits traits(void)
{
    TegraVICTraits t;
    memset(&t, 0, sizeof(t));
    t.in = t.out = kMeasured;
    // The dihedral group as VIC implements it if flips come after the transpose (the self-test measures the real one).
    static const uint8_t flips[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                        {1, 0, 1}, {1, 1, 1}, {0, 0, 1}, {0, 1, 1}};
    memcpy(t.vicFlips, flips, sizeof(flips));
    t.rotations = 0xff;
    t.subRects = 0xff;
    t.bilinear = 1;
    t.sourcePitchAlign = t.destinationPitchAlign = 64;
    t.fillThroughMatrix = 1;
    t.yccIn = TEGRA_VIC_YCC_IN_RGB | TEGRA_VIC_YCC_IN_COPY;
    return t;
}

static TegraVICImage image(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch, uint64_t base)
{
    TegraVICImage im;
    memset(&im, 0, sizeof(im));
    im.format = tegra_vic_format(fourcc);
    im.width = w;
    im.height = h;
    im.pitch[0] = pitch;
    im.rows[0] = h;
    im.base[0] = base;
    im.span[0] = (uint64_t)pitch * h;
    if (im.format && im.format->planes > 1) {
        im.pitch[1] = pitch;
        im.rows[1] = h / 2;
        im.base[1] = base + (uint64_t)pitch * h;
        im.span[1] = (uint64_t)pitch * h / 2;
    }
    im.standard = TEGRA_VIC_YCC_709;
    return im;
}

static uint32_t methodValue(const TegraVICJob *j, uint32_t m, int *found)
{
    for (uint32_t i = 0; i < j->count; i++)
        if (j->method[i] == m) {
            *found = 1;
            return j->value[i];
        }
    *found = 0;
    return 0;
}

static void jobs(void)
{
    const TegraVICTraits t = traits();
    // Screen Recording's clone: the 720x1280 LCD (BGRA, 2880-byte rows) a quarter turn into a 1280x720 '420f'.
    TegraVICImage src = image(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'), 720, 1280, 2880, 0x10000000);
    TegraVICImage dst = image(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 1280, 720, 1280, 0x18000000);
    TegraVICOp op;
    memset(&op, 0, sizeof(op));
    op.srcW = 720u << 16;
    op.srcH = 1280u << 16;
    op.dstW = 1280;
    op.dstH = 720;
    op.transform = TEGRA_VIC_ROT90;
    TegraVICJob job;
    const char *why = tegra_vic_build(&src, &dst, &op, &t, 0x80001000, 0x80002000, &job);
    CHECK(!why, "recording job refused: %s", why);
    int f = 0;
    CHECK(methodValue(&job, TEGRA_VIC_M_SET_APPLICATION_ID, &f) == 1 && f, "application id");
    CHECK(methodValue(&job, TEGRA_VIC_M_SET_CONTROL_PARAMS, &f) == (0x61u << 16 | 0x101) && f, "control params");
    CHECK(methodValue(&job, TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET, &f) == 0x800010 && f, "config offset");
    CHECK(methodValue(&job, TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET, &f) == 0x800020 && f, "filter offset");
    CHECK(methodValue(&job, TEGRA_VIC_M_OUTPUT_LUMA, &f) == 0x180000 && f, "output luma");
    CHECK(methodValue(&job, TEGRA_VIC_M_OUTPUT_CHROMA_U, &f) == (0x18000000u + 1280 * 720) >> 8 && f, "output chroma");
    CHECK(methodValue(&job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, &f) == 0x100000 && f, "source luma");
    methodValue(&job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, &f);
    CHECK(!f, "a BGRA source has no chroma method");
    CHECK(job.method[job.count - 1] == TEGRA_VIC_M_EXECUTE && job.value[job.count - 1] == TEGRA_VIC_EXECUTE_AWAKEN,
          "EXECUTE last");
    const uint8_t *c = job.config;
    const uint32_t s = TEGRA_VIC_SLOTS, sf = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE) == 1 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X) == 1 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_Y) == 0,
          "rotation: transpose and flip X");
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT) == 1279 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM) == 719,
          "target rectangle");
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT) == TEGRA_VIC_T_Y8___U8V8_N420 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 1279 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH) == 639 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT) == 359,
          "output surface");
    CHECK(TEGRA_VIC_GET(c, sf, SURF_FORMAT) == TEGRA_VIC_T_A8R8G8B8 && TEGRA_VIC_GET(c, sf, SURF_LUMA_WIDTH) == 719 &&
              TEGRA_VIC_GET(c, sf, SURF_LUMA_HEIGHT) == 1279,
          "slot surface");
    CHECK(TEGRA_VIC_GET(c, s, SLOT_SOURCE_RIGHT) == 719u << 16 && TEGRA_VIC_GET(c, s, SLOT_SOURCE_BOTTOM) == 1279u
                                                                                                                 << 16,
          "source rectangle");
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE) == 1 &&
              TEGRA_VIC_GET(c, s + TEGRA_VIC_SLOT_MATRIX, MATRIX_ENABLE) == 0,
          "RGB in, Y'CbCr out: the output matrix only");
    // The same job with rectangles and the output surface before the transpose (hekate's programming, the convention
    // the self-test tries first): the 1280x720 '420f' described as the 720x1280 picture VIC composes, its planes'
    // memory geometry with width and height exchanged (720 rows of 1280 luma bytes are LumaWidth 719, LumaHeight 1279;
    // 360 rows of 640 CbCr pairs ChromaWidth 359, ChromaHeight 639), the slot's destination rectangle the target's.
    TegraVICTraits pre = t;
    pre.preTransposeRects = pre.preTransposeSurface = 1;
    why = tegra_vic_build(&src, &dst, &op, &pre, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT) == 1279,
          "pre-transpose rectangles and surface size");
    CHECK(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH) == 359 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT) == 639,
          "pre-transpose plane geometry: luma %llux%llu, chroma %llux%llu",
          (unsigned long long)TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH),
          (unsigned long long)TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT),
          (unsigned long long)TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH),
          (unsigned long long)TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT));
    CHECK(TEGRA_VIC_GET(job.config, s, SLOT_DEST_RIGHT) == 719 &&
              TEGRA_VIC_GET(job.config, s, SLOT_DEST_BOTTOM) == 1279,
          "pre-transpose slot destination rectangle");
    // The two measured apart: rectangles before the transpose with the surface after it, and the reverse.
    pre.preTransposeSurface = 0;
    why = tegra_vic_build(&src, &dst, &op, &pre, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT) == 719,
          "pre-transpose rectangles, post-transpose surface");
    pre.preTransposeRects = 0, pre.preTransposeSurface = 1;
    why = tegra_vic_build(&src, &dst, &op, &pre, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 719,
          "post-transpose rectangles, pre-transpose surface");
    // A job without the transpose keeps its geometry under either convention.
    TegraVICOp flat = op;
    flat.transform = 0;
    flat.srcW = 720u << 16;
    flat.srcH = 1280u << 16;
    flat.dstW = 720;
    flat.dstH = 1280;
    TegraVICImage tall = image(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 720, 1280, 768, 0x18000000);
    pre.preTransposeRects = 1;
    why = tegra_vic_build(&src, &tall, &flat, &pre, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT) == 719 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 767 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT) == 1279 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH) == 383 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT) == 639,
          "untransposed geometry under the pre-transpose conventions: %s", why ? why : "fields");

    // A fill (Core Animation's border clears): no slot, the background colour.
    memset(&op, 0, sizeof(op));
    op.fill = 1;
    op.dstX = 0;
    op.dstY = 0;
    op.dstW = 1280;
    op.dstH = 720;
    op.fill10[0] = 64;
    op.fill10[1] = op.fill10[2] = 512;
    why = tegra_vic_build(NULL, &dst, &op, &t, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS, SLOT_ENABLE) == 0 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R) == 64,
          "fill job");
    methodValue(&job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, &f);
    CHECK(!f, "a fill reads no surface");

    // Refusals: a valid recording job with one thing wrong, and the reason the checker gives.
    static const struct {
        int what;
        const char *why;
    } cases[] = {
        {0, "plane extends past its mapping"},
        {1, "row pitch the engine cannot address"},
        {2, "plane not 256-byte aligned"},
        {3, "4:2:0 surface with an odd size"},
        {4, "destination rectangle outside the destination"},
        {5, "source rectangle outside the source"},
        {6, "transform this engine has not proven"},
        {7, "upscale past 16x"},
        {8, "downscale past 16x"},
        {9, "pixel format the scaler does not take"},
        {10, "surface size outside 2..8192"},
        {11, "row pitch shorter than a row"},
        {12, "4:2:0 destination rectangle on odd coordinates"},
        {13, "source rectangle under one pixel"},
        {14, "transform outside the 8 flips and rotations"},
        {15, "plane shorter than the surface"},
        {16, "configuration not 256-byte aligned"},
        {17, "destination sub-rectangle this transform has not proven"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TegraVICImage s2 = src, d2 = dst;
        TegraVICOp o2;
        memset(&o2, 0, sizeof(o2));
        o2.srcW = 720u << 16;
        o2.srcH = 1280u << 16;
        o2.dstW = 1280;
        o2.dstH = 720;
        o2.transform = TEGRA_VIC_ROT90;
        TegraVICTraits t2 = t;
        uint64_t cfg = 0x80001000;
        switch (cases[i].what) {
        case 0:
            s2.span[0] -= 1;
            break;
        case 1:
            s2.pitch[0] = 2880 + 32;
            break;
        case 2:
            d2.base[1] += 64;
            break;
        case 3:
            d2.width = 1279;
            break;
        case 4:
            o2.dstX = 2;
            break;
        case 5:
            o2.srcY = 1u << 16;
            break;
        case 6:
            t2.rotations = 0xef;
            break;
        case 7:
            o2.srcW = 40u << 16;
            o2.srcH = 1280u << 16;
            o2.dstH = 720;
            o2.transform = 0;
            o2.dstW = 1280;
            o2.srcH = 40u << 16;
            break;
        case 8:
            o2.transform = 0;
            o2.srcW = 720u << 16;
            o2.srcH = 1280u << 16;
            o2.dstW = 40;
            o2.dstH = 40;
            break;
        case 9:
            s2.format = tegra_vic_format(TEGRA_VIC_FOURCC('x', '4', '2', '0'));
            break;
        case 10:
            s2.width = 1;
            break;
        case 11:
            s2.pitch[0] = 2048;
            break;
        case 12:
            o2.dstX = 1;
            o2.dstW = 1278;
            break;
        case 13:
            o2.srcW = 0x8000;
            break;
        case 14:
            o2.transform = 8;
            break;
        case 15:
            d2.rows[1] = 359;
            break;
        case 16:
            cfg = 0x80001080;
            break;
        case 17:
            t2.subRects = 0x01;
            o2.dstY = 2;
            o2.dstH = 718;
            break;
        }
        why = tegra_vic_build(&s2, &d2, &o2, &t2, cfg, 0x80002000, &job);
        CHECK(why && !strcmp(why, cases[i].why), "case %d: wanted \"%s\", got \"%s\"", cases[i].what, cases[i].why,
              why ? why : "accepted");
    }
}

// --- requests -------------------------------------------------------------------------------------------------------
static TegraVICSurfaceDesc desc(uint32_t fourcc, uint32_t w, uint32_t h, uint32_t pitch0, uint32_t pitch1)
{
    TegraVICSurfaceDesc d;
    memset(&d, 0, sizeof(d));
    d.fourcc = fourcc;
    d.width = w;
    d.height = h;
    const TegraVICFormat *f = tegra_vic_format(fourcc);
    d.planes = f && f->planes > 1 ? 2 : 0;
    d.planeBytesPerRow[0] = pitch0;
    d.planeHeight[0] = h;
    d.planeWidth[0] = w;
    if (d.planes == 2) {
        d.planeOffset[1] = ((uint64_t)pitch0 * h + 0xfff) & ~0xfffull;
        d.planeBytesPerRow[1] = pitch1;
        d.planeHeight[1] = h / 2;
        d.planeWidth[1] = w / 2;
    }
    d.length = (tegra_vic_surface_extent(&d) + 0x3fff) & ~0x3fffull;
    return d;
}

// What IOSurfaceAccelerator.framework sends for Core Animation's clone copy (TransformSurface, options Transform,
// NearestFilter false, ColorRemapMode 0; DitherControl's default 0x2000, FixUpscaling forced for the transform API,
// the destination's chroma siting) with the callback msr_transfer_complete.
static TegraVICRequest cloneRequest(uint32_t transform)
{
    TegraVICRequest r;
    memset(&r, 0, sizeof(r));
    r.src_id = 0x11;
    r.dst_id = 0x22;
    r.cb_fn = 0x18966cbb0;
    r.cb_refcon = 0x123456;
    r.flags = transform | 0x2000 | 0x1000 | (1ull << 15);
    return r;
}

static const char *buildAll(const TegraVICPlan *plan, const TegraVICSurfaceDesc *sd, const TegraVICSurfaceDesc *dd,
                            const TegraVICTraits *t, uint32_t site, TegraVICJob *last)
{
    TegraVICImage src, dst;
    memset(&src, 0, sizeof(src));
    const char *why = tegra_vic_image_from(dd, 0x18000000, &dst);
    dst.chromaSite = site;
    if (!why && plan->readsSource)
        why = tegra_vic_image_from(sd, 0x10000000, &src);
    for (uint32_t i = 0; !why && i < plan->count; i++)
        why = tegra_vic_build(plan->op[i].fill ? NULL : &src, &dst, &plan->op[i], t, 0x80001000, 0x80002000, last);
    return why;
}

static void requests(void)
{
    const TegraVICTraits t = traits();
    TegraVICPlan plan;
    TegraVICJob job;
    uint32_t kind = 9;
    // Screen Recording: the 720x1280 LCD (BGRA) into replayd's 720x1280 '420f' writeback buffer, Transform 0.
    TegraVICSurfaceDesc lcd = desc(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'), 720, 1280, 2880, 0);
    TegraVICSurfaceDesc wb = desc(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 720, 1280, 768, 768);
    TegraVICRequest r = cloneRequest(0);
    const char *why = tegra_vic_plan(&r, &lcd, &wb, &plan, &kind);
    CHECK(!why && kind == TEGRA_VIC_PLAN_OK && plan.count == 1 && plan.readsSource && plan.callback,
          "recording request: %s", why ? why : "plan");
    CHECK(plan.op[0].srcW == 720u << 16 && plan.op[0].srcH == 1280u << 16 && plan.op[0].dstW == 720 &&
              plan.op[0].dstH == 1280 && plan.op[0].transform == 0 && !plan.op[0].nearest,
          "recording request: empty rectangles are the whole surfaces");
    why = buildAll(&plan, &lcd, &wb, &t, TEGRA_VIC_REQ_CHROMA_SITE(r.flags), &job);
    CHECK(!why, "recording jobs: %s", why);
    CHECK(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ) == 0 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT) == 1 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 767,
          "recording jobs: chroma siting Left, 768-byte luma rows");
    // The same with VIC's 256-byte source pitch rule: the LCD's 2880-byte rows are refused unless they are bounced.
    TegraVICTraits strict = t;
    strict.sourcePitchAlign = 256;
    why = buildAll(&plan, &lcd, &wb, &strict, 1, &job);
    CHECK(why && !strcmp(why, "row pitch the engine cannot address"), "256-byte pitch rule: %s", why ? why : "ok");

    // AirPlay-style clone: landscape 1280x720 '420f' from the portrait LCD, a quarter turn, aspect-fit rectangle.
    TegraVICSurfaceDesc air = desc(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 1280, 720, 1280, 1280);
    r = cloneRequest(4);
    r.dst_x = 0;
    r.dst_y = 0;
    r.dst_w = 1280;
    r.dst_h = 720;
    why = tegra_vic_plan(&r, &lcd, &air, &plan, &kind);
    CHECK(!why && plan.op[0].transform == 4, "rotated clone plan: %s", why ? why : "transform");
    why = buildAll(&plan, &lcd, &air, &t, 1, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE) == 1, "rotated clone jobs: %s", why);

    // Border clear (BlitSurface with WriteOnlyMode 255/0/0/0): one fill, no source, black in the destination's
    // encoding. With the background converted by the driver (the self-test's other branch): full-range black is
    // Y 0, Cb 128, Cr 128 after the engine's >> 2.
    memset(&r, 0, sizeof(r));
    r.dst_id = 0x22;
    r.flags = TEGRA_VIC_REQ_WRITE_ONLY | 0x1000 | 0x2000;
    r.wo_alpha = 255;
    r.dst_x = 0;
    r.dst_y = 0;
    r.dst_w = 720;
    r.dst_h = 100;
    r.cb_fn = 1;
    why = tegra_vic_plan(&r, NULL, &wb, &plan, &kind);
    CHECK(!why && plan.count == 1 && plan.op[0].fill && !plan.readsSource, "border clear plan: %s", why ? why : "");
    r.dst_y = 3;
    r.dst_h = 99;
    why = tegra_vic_plan(&r, NULL, &wb, &plan, &kind);
    CHECK(!why && plan.op[0].dstY == 2 && plan.op[0].dstH == 100, "4:2:0 fill widened to even rows");
    TegraVICTraits conv = t;
    conv.fillThroughMatrix = 0;
    why = buildAll(&plan, NULL, &wb, &conv, 1, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R) >> 2 == 0 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G) >> 2 == 128 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B) >> 2 == 128 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE) == 0,
          "border clear: black converted");
    why = buildAll(&plan, NULL, &wb, &t, 1, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R) == 0 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE) == 1,
          "border clear: black through the output matrix");

    // ZeroFill and BorderFill come first, the transform last.
    r = cloneRequest(0);
    r.flags |= TEGRA_VIC_REQ_ZERO_FILL | TEGRA_VIC_REQ_BORDER_FILL;
    r.border_w = 720;
    r.border_h = 2;
    r.border_alpha = 255;
    why = tegra_vic_plan(&r, &lcd, &wb, &plan, &kind);
    CHECK(!why && plan.count == 3 && plan.op[0].fill && plan.op[1].fill && !plan.op[2].fill, "fills first: %s",
          why ? why : "order");

    // Refusals before anything is queued: what VIC does not do is "unsupported" (callers fall back), the rest bad.
    static const struct {
        int what, kind;
        const char *why;
    } cases[] = {
        {0, TEGRA_VIC_PLAN_BAD, "transform value outside the 8 flips and rotations"},
        {1, TEGRA_VIC_PLAN_UNSUPPORTED, "histogram (none on VIC)"},
        {2, TEGRA_VIC_PLAN_UNSUPPORTED, "HDR or film grain (capabilities say no)"},
        {3, TEGRA_VIC_PLAN_UNSUPPORTED, "colour remap (capabilities say no)"},
        {5, TEGRA_VIC_PLAN_UNSUPPORTED, "pixel format the scaler does not take"},
        {6, TEGRA_VIC_PLAN_UNSUPPORTED, "protected source into a less protected destination"},
        {7, TEGRA_VIC_PLAN_UNSUPPORTED, "alpha override into a surface with alpha"},
        {8, TEGRA_VIC_PLAN_UNSUPPORTED, "HDR or film grain (capabilities say no)"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TegraVICRequest q = cloneRequest(0);
        TegraVICSurfaceDesc sd = lcd, dd = wb;
        switch (cases[i].what) {
        case 0:
            q.flags |= 0x8;
            break;
        case 1:
            q.flags |= TEGRA_VIC_REQ_HISTOGRAM;
            break;
        case 2:
            q.flags |= TEGRA_VIC_REQ_HDR;
            break;
        case 3:
            q.flags |= 1ull << 36;
            break;
        case 4:
            q.src_event_port = 0x1503;
            break;
        case 5:
            dd.fourcc = TEGRA_VIC_FOURCC('x', '4', '2', '0');
            break;
        case 6:
            sd.protection = 1;
            break;
        case 7:
            q.flags |= TEGRA_VIC_REQ_ALPHA_OVERRIDE;
            dd = lcd;
            break;
        case 8:
            q.flags |= TEGRA_VIC_REQ_FILM_GRAIN;
            break;
        case 9:
            q.dst_event_port = 0x1603;
            break;
        }
        why = tegra_vic_plan(&q, &sd, &dd, &plan, &kind);
        CHECK(why && !strcmp(why, cases[i].why) && kind == (uint32_t)cases[i].kind, "request case %d: got \"%s\"",
              cases[i].what, why ? why : "accepted");
    }
    // Geometry the plan accepts but the checker refuses: a source rectangle past the source, a plane past its memory.
    r = cloneRequest(0);
    r.src_x = 1u << 16;
    r.src_w = 720u << 16;
    r.src_h = 1280u << 16;
    why = tegra_vic_plan(&r, &lcd, &wb, &plan, &kind);
    CHECK(!why, "offset source plan");
    why = buildAll(&plan, &lcd, &wb, &t, 1, &job);
    CHECK(why && !strcmp(why, "source rectangle outside the source"), "source past the edge: %s", why ? why : "ok");
    // A unit whose self-test did not prove reading 4:2:0: such sources are refused (420f -> BGRA, 420f -> 420f).
    TegraVICTraits noYcc = t;
    noYcc.yccIn = 0;
    TegraVICSurfaceDesc nv = desc(TEGRA_VIC_FOURCC('4', '2', '0', 'f'), 1280, 720, 1280, 1280);
    TegraVICSurfaceDesc rgbOut = desc(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'), 1280, 720, 5120, 0);
    r = cloneRequest(0);
    why = tegra_vic_plan(&r, &nv, &rgbOut, &plan, &kind);
    CHECK(!why, "420f -> BGRA plan");
    why = buildAll(&plan, &nv, &rgbOut, &noYcc, 1, &job);
    CHECK(why && !strcmp(why, "4:2:0 conversion this engine has not proven"), "unproven 4:2:0 -> RGB: %s",
          why ? why : "ok");
    why = buildAll(&plan, &nv, &rgbOut, &t, 1, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX, MATRIX_ENABLE) == 1,
          "proven 4:2:0 -> RGB uses the slot matrix: %s", why ? why : "ok");
    why = tegra_vic_plan(&r, &nv, &air, &plan, &kind);
    CHECK(!why, "420f -> 420f plan");
    why = buildAll(&plan, &nv, &air, &noYcc, 1, &job);
    CHECK(why && !strcmp(why, "4:2:0 copy this engine has not proven"), "unproven 4:2:0 copy: %s", why ? why : "ok");
    why = buildAll(&plan, &nv, &air, &t, 1, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX, MATRIX_ENABLE) == 0 &&
              TEGRA_VIC_GET(job.config, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE) == 0,
          "proven 4:2:0 copy passes through: %s", why ? why : "ok");
    TegraVICSurfaceDesc shortMem = wb;
    shortMem.length = 768 * 1280;
    TegraVICImage im;
    why = tegra_vic_image_from(&shortMem, 0x18000000, &im);
    CHECK(why && !strcmp(why, "planes extend past the surface's memory"), "short memory: %s", why ? why : "ok");
}

// NVDEC's pictures (TegraVICPresent.h): a 1920x1080 H.264 picture as NVDEC leaves it (pitch 1920, 1088 block-aligned
// rows, chroma at 1920 x 1088) copied 1:1 into a 2048-byte-row '420v' buffer. The source surface takes averne's
// block-linear setup (16Bx2, 2 GOBs a block, 32Bx8 cache, the planes' whole blocks as their heights) and the copy no
// matrix; the checker refuses what VIC cannot read as block linear.
static void blockLinear(void)
{
    TegraVICTraits t = traits();
    t.blockLinearIn = 1;
    const uint32_t fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    TegraVICImage src = image(fourcc, 1920, 1080, 1920, 0x10000000);
    src.rows[0] = 1088;
    src.rows[1] = 544;
    src.base[1] = 0x10000000 + 1920 * 1088;
    src.span[0] = 1920 * 1088;
    src.span[1] = 1920 * 544;
    src.blockLinear = 1;
    src.blockHeightLog2 = 1;
    const TegraVICImage dst = image(fourcc, 1920, 1080, 2048, 0x18000000);
    TegraVICOp op;
    memset(&op, 0, sizeof(op));
    op.srcW = 1920u << 16;
    op.srcH = 1080u << 16;
    op.dstW = 1920;
    op.dstH = 1080;
    op.nearest = 1;
    TegraVICJob job;
    const char *why = tegra_vic_build(&src, &dst, &op, &t, 0x80001000, 0x80002000, &job);
    CHECK(!why, "NVDEC picture refused: %s", why ? why : "");
    const uint8_t *c = job.config;
    const uint32_t sf = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    CHECK(TEGRA_VIC_GET(c, sf, SURF_BLK_KIND) == TEGRA_VIC_BLK_16Bx2 && TEGRA_VIC_GET(c, sf, SURF_BLK_HEIGHT) == 1 &&
              TEGRA_VIC_GET(c, sf, SURF_CACHE_WIDTH) == TEGRA_VIC_CACHE_32Bx8,
          "block-linear kind, height or cache");
    CHECK(TEGRA_VIC_GET(c, sf, SURF_WIDTH) == 1919 && TEGRA_VIC_GET(c, sf, SURF_HEIGHT) == 1079 &&
              TEGRA_VIC_GET(c, sf, SURF_LUMA_WIDTH) == 1919 && TEGRA_VIC_GET(c, sf, SURF_LUMA_HEIGHT) == 1087 &&
              TEGRA_VIC_GET(c, sf, SURF_CHROMA_WIDTH) == 959 && TEGRA_VIC_GET(c, sf, SURF_CHROMA_HEIGHT) == 543,
          "block-linear geometry: luma %llu rows, chroma %llu rows",
          (unsigned long long)TEGRA_VIC_GET(c, sf, SURF_LUMA_HEIGHT),
          (unsigned long long)TEGRA_VIC_GET(c, sf, SURF_CHROMA_HEIGHT));
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND) == TEGRA_VIC_BLK_PITCH &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH) == 2047,
          "the output stays pitch linear");
    CHECK(TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX, MATRIX_ENABLE) == 0 &&
              TEGRA_VIC_GET(c, TEGRA_VIC_OUT_MATRIX, MATRIX_ENABLE) == 0,
          "the copy passes the samples through");
    int f;
    CHECK(methodValue(&job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, &f) == (0x10000000u + 1920 * 1088) >> 8 && f,
          "source chroma");
    // A pitch-linear source keeps its old setup (no blocks, the 64Bx4 cache, its height as the luma rows).
    TegraVICImage pitch = image(fourcc, 1920, 1080, 2048, 0x10000000);
    why = tegra_vic_build(&pitch, &dst, &op, &t, 0x80001000, 0x80002000, &job);
    CHECK(!why && TEGRA_VIC_GET(job.config, sf, SURF_BLK_KIND) == TEGRA_VIC_BLK_PITCH &&
              TEGRA_VIC_GET(job.config, sf, SURF_CACHE_WIDTH) == TEGRA_VIC_CACHE_64Bx4 &&
              TEGRA_VIC_GET(job.config, sf, SURF_LUMA_HEIGHT) == 1079,
          "pitch-linear source setup changed: %s", why ? why : "");

    struct {
        int what;
        const char *why;
    } cases[] = {
        {0, "block-linear source this engine has not proven"},
        {1, "row pitch the engine cannot address"},
        {2, "block-linear plane of partial blocks"},
        {3, "plane extends past its mapping"},
        {4, "block-linear destination this engine has not proven"},
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TegraVICTraits tt = t;
        TegraVICImage s = src, d = dst;
        switch (cases[i].what) {
        case 0:
            tt.blockLinearIn = 0;
            break;
        case 1:
            s.pitch[0] = s.pitch[1] = 1952;
            break;  // 32 bytes past a GOB
        case 2:
            s.rows[0] = 1080, s.rows[1] = 540;
            break;
        case 3:
            s.span[1] = 1920 * 540;
            break;
        case 4:
            tt.blockLinearOut = 0;
            d.blockLinear = 1;
            break;
        }
        why = tegra_vic_build(&s, &d, &op, &tt, 0x80001000, 0x80002000, &job);
        CHECK(why && !strcmp(why, cases[i].why), "block-linear case %d: wanted \"%s\", got \"%s\"", cases[i].what,
              cases[i].why, why ? why : "accepted");
    }
}

int main(void)
{
    layout();
    colour();
    idealReference();
    jobs();
    requests();
    blockLinear();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS vic (%d checks)\n", checks);
    return 0;
}
