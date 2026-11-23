// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC 4.0's colour stages, and which colour-space changes of an IOSurfaceAccelerator transform TegraVIC gives them.
// Plain C, header only, included by TegraVICConfig.h (which writes the stages into a job) and the host tests.
//
// The stages (TRM 15.2 Figure 28, 15.3.2): an RGB slot is linearised by its degamma curve ahead of the scalers ("ARGB
// Degamma"), a Y'CbCr slot after its colour matrix ("Color Conversion + Degamma"); the slot's gamut matrix then works
// in linear light ("Gamut Remap"), clipping negative results when ClipEnabled is set; the slots blend; the blended
// output takes the regamma curve. Fields: SlotConfig DegammaMode and ClipEnabled (Table 74), slotStruct
// gamutMatrixStruct (Table 73, a MatrixStruct like Table 71: S12.8 coefficients, the sum shifted right by r_shift,
// 15.3.14), OutputConfig RegammaMode (Table 69). Curves (Table 64): none, sRGB, Rec.709, and value 3, named RESERVED,
// described as Rec.2020.
//
// What Apple's own conversions do, measured on an M4 Max with macOS 27.2.1 (VTPixelTransferSession, scaler hardware and
// Metal each alone, every 8-bit RGB value; c38-audit/vic-colour/probe): ITU_R_709_2 is decoded as a 1.961 power with a
// short linear toe, which none of VIC's curves is, so every transfer-function change is refused. Between sRGB-encoded
// RGB surfaces both backends are within one code of the float conversion for 709 <-> Display P3 and 709 or Display P3
// -> BT.2020. Out of BT.2020 they clip differently from each other; the hardware passes SMPTE C and EBU 3213 through as
// 709 where Metal converts them; Y'CbCr sides disagree by more than a code between the two; and the hardware scales
// colour-managed RGB in linear light where Metal scales in gamma space. The host model (test/vic_model_color.h) writes
// 709 -> Display P3 and 709 or Display P3 -> BT.2020 within one code of both, however VIC widens RGB and quantizes the
// regamma; Display P3 -> 709 only for one of those (2 to 3 codes if RGB enters as x << 2 | x >> 6 and the regamma
// truncates), so it is refused with the rest.
#ifndef TEGRA_VIC_COLOR_H
#define TEGRA_VIC_COLOR_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_VIC_GAMMA_NONE = 0,
    TEGRA_VIC_GAMMA_SRGB = 1,
    TEGRA_VIC_GAMMA_REC709 = 2,
    TEGRA_VIC_GAMMA_REC2020 = 3,
};

enum {
    SLOT_CLIP_ENABLED_BIT = 174,
    SLOT_CLIP_ENABLED_WIDTH = 1,
    SLOT_DEGAMMA_BIT = 183,
    SLOT_DEGAMMA_WIDTH = 2,
    OUT_REGAMMA_BIT = 46,
    OUT_REGAMMA_WIDTH = 2,
};

// ITU-T H.273 code points, as IOSurface's bulk attachments hold a surface's tags (0 and 2: none given).
enum {
    TEGRA_VIC_TF_709 = 1,
    TEGRA_VIC_TF_601 = 6,
    TEGRA_VIC_TF_SRGB = 13,
    TEGRA_VIC_TF_2020_10 = 14,
    TEGRA_VIC_TF_2020_12 = 15,
    TEGRA_VIC_TF_PQ = 16,
    TEGRA_VIC_TF_HLG = 18,
    TEGRA_VIC_PRIMARIES_709 = 1,
    TEGRA_VIC_PRIMARIES_EBU = 5,
    TEGRA_VIC_PRIMARIES_SMPTE_C = 6,
    TEGRA_VIC_PRIMARIES_2020 = 9,
    TEGRA_VIC_PRIMARIES_P3_D65 = 12,
};

// Linear RGB of `from`'s primaries to `to`'s, both D65 white: c[r][i] = round(gain * 2^(8 + shift)), `shift` the
// largest that keeps every coefficient inside 20 signed bits, each row summing to exactly 1.0. Derived from the
// primaries' chromaticities by test:tegra_vic_color_test, which checks every entry.
typedef struct {
    uint8_t from, to, shift;
    int32_t c[3][3];
} TegraVICGamut;

static const TegraVICGamut kTegraVICGamuts[] = {
    {TEGRA_VIC_PRIMARIES_709,
     TEGRA_VIC_PRIMARIES_P3_D65,
     11,
     {{431207, 93081, 0}, {17403, 506885, 0}, {8956, 37957, 477375}}},
    {TEGRA_VIC_PRIMARIES_709,
     TEGRA_VIC_PRIMARIES_2020,
     11,
     {{328940, 172639, 22709}, {36227, 482104, 5957}, {8594, 46144, 469550}}},
    {TEGRA_VIC_PRIMARIES_P3_D65,
     TEGRA_VIC_PRIMARIES_2020,
     11,
     {{395226, 104122, 24940}, {23983, 493762, 6543}, {-635, 9228, 515695}}},
};

static inline uint32_t tegra_vic_color_code(uint32_t code)
{
    return code == 2 ? 0 : code;
}

static inline uint32_t tegra_vic_color_curve(uint32_t transfer)
{
    return transfer == TEGRA_VIC_TF_601 || transfer == TEGRA_VIC_TF_2020_10 || transfer == TEGRA_VIC_TF_2020_12
               ? TEGRA_VIC_TF_709
               : transfer;
}

// Apple's scaler passes EBU 3213 and SMPTE C through as BT.709.
static inline uint32_t tegra_vic_color_primaries(uint32_t code)
{
    code = tegra_vic_color_code(code);
    return code == TEGRA_VIC_PRIMARIES_EBU || code == TEGRA_VIC_PRIMARIES_SMPTE_C ? TEGRA_VIC_PRIMARIES_709 : code;
}

// Decides a transform's colour from the surfaces' transfer and primaries code points and whether both sides are 8-bit
// RGB: *gamut the change VIC takes (with sRGB degamma and regamma), null for none. A side without a tag converts
// nothing for that tag, as VideoToolbox clears an unset one before the scaler sees it. Returns 0, or why the change is
// refused (the caller converts it some other way).
static inline const char *tegra_vic_color_plan(uint32_t srcTransfer, uint32_t srcPrimaries, uint32_t dstTransfer,
                                               uint32_t dstPrimaries, int rgb8, const TegraVICGamut **gamut)
{
    const uint32_t st = tegra_vic_color_curve(tegra_vic_color_code(srcTransfer));
    const uint32_t dt = tegra_vic_color_curve(tegra_vic_color_code(dstTransfer));
    const uint32_t sp = tegra_vic_color_primaries(srcPrimaries);
    const uint32_t dp = tegra_vic_color_primaries(dstPrimaries);
    *gamut = NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
    if (st && dt && st != dt)
        return st == TEGRA_VIC_TF_PQ || st == TEGRA_VIC_TF_HLG || dt == TEGRA_VIC_TF_PQ || dt == TEGRA_VIC_TF_HLG
                   ? "PQ or HLG transfer change (no such curve on VIC)"
                   : "transfer function change (VIC's curves are not Apple's)";
    // Core Animation's copies for a detached layer (IOMFBDisplay::detach_layer) carry the source's
    // SurfaceColorPrimaries, which bucket every tag but P3_D65 and 2020 as BT.709 and write that bucket back as 709: no
    // change is asked.
    if (!sp || !dp || sp == dp ||
        (dp == TEGRA_VIC_PRIMARIES_709 && sp != TEGRA_VIC_PRIMARIES_P3_D65 && sp != TEGRA_VIC_PRIMARIES_2020))
        return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
    if (st != TEGRA_VIC_TF_SRGB || dt != TEGRA_VIC_TF_SRGB)
        return "gamut change outside sRGB-encoded surfaces";
    if (!rgb8)
        return "gamut change with a side that is not 8-bit RGB";
    for (uint32_t i = 0; i < sizeof(kTegraVICGamuts) / sizeof(kTegraVICGamuts[0]); i++)
        if (kTegraVICGamuts[i].from == sp && kTegraVICGamuts[i].to == dp) {
            *gamut = &kTegraVICGamuts[i];
            return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
        }
    return "primaries change VIC does not take";
}

#ifdef __cplusplus
}
#endif
#endif
