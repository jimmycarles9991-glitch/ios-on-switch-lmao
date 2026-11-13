// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC 4.0 (Tegra X1's Video Image Compositor, host1x class 0x5D) job generation: the configuration structure the
// falcon firmware reads, the class methods that point it at the surfaces, the colour matrices, and the checker that
// bounds every byte the engine may read or write. Plain C, header only: TegraVIC (kernel C++) and the host test
// (drivers/TegraVIC/test/vic_host.c, test:tegra_vic_test) compile the same code.
//
// Sources (written from them; layouts checked bit for bit by the host test against libdrm's MIT-licensed vic40.h):
//   libdrm tests/tegra/vic40.h, vic40.c, vic.h (NVIDIA, MIT): ConfigStruct and its sub-structures, the class methods
//   SET_APPLICATION_ID 0x200, EXECUTE 0x300, SET_SURFACE0_SLOT0_LUMA/CHROMA_U 0x400/0x404, SET_CONTROL_PARAMS 0x704,
//   SET_CONFIG_STRUCT_OFFSET 0x708, SET_OUTPUT_SURFACE_LUMA 0x720; pixel formats; cache widths.
//   cyndis/vaapi-tegra-driver engines/vic.cpp, engine_headers/vic04.h (NVIDIA, MIT): SET_FILTER_STRUCT_OFFSET 0x70c,
//   SET_OUTPUT_SURFACE_CHROMA_U 0x724, the 4:2:0 chroma plane geometry, the colour matrix encoding (coefficient
//   1.0 = 1 << (r_shift + 8), offsets in 1/256 of a 10-bit step).
//   switch-frame-tap tools/vic_csc.py (measured on a T210B01 VIC, 528 values reproduced exactly; read, not copied):
//   the 10-bit pipeline (in10 = in8 << 2, out8 = out10 >> 2), both matrix stages truncate, and which pipeline channel
//   each NV12 byte lands in. The unit's boot self-test confirmed these (kTegraVICTraits).
//   averne's FFmpeg nvtegra (libavutil/clb0b6.h, hwcontext_nvtegra.c; read, not copied): CONTROL_PARAMS GPTIMER_ON
//   and FALCON_CONTROL, PipeConfig downsample 1.0 = 4 (U9.2), the chroma siting values, NV12 = T_Y8___U8V8_N420.
//   CTCaer's hekate bdk/display/vic.c (GPLv2; read, not copied), the one VIC transpose known to work on this hardware
//   (Nyx's 1280x720 frame turned onto the 720x1280 panel): a transposed job is described before the transpose, the
//   target rectangle and the whole OutputSurfaceConfig (OutSurfaceWidth/Height and OutLumaWidth/Height 1279/719, the
//   source's), and the engine writes a 720-pixel-stride, 1280-row picture: composed pixel (x, y) lands in memory row x,
//   column y, so LumaHeight + 1 is the row stride and LumaWidth + 1 the rows. The unit's diag lines showed the
//   same law: with LumaHeight 31 (the rows, post-transpose) a 256-byte-pitch 16x32 output got a 128-byte row stride.
#ifndef TEGRA_VIC_CONFIG_H
#define TEGRA_VIC_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "TegraVICFormats.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Class methods (byte offsets in class 0xB0B6) -----------------------------------------------------------------
enum {
    TEGRA_VIC_M_SET_APPLICATION_ID = 0x200,
    TEGRA_VIC_M_EXECUTE = 0x300,
    TEGRA_VIC_M_SURFACE0_SLOT0_LUMA = 0x400,
    TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U = 0x404,
    TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_V = 0x408,
    TEGRA_VIC_M_SET_CONTROL_PARAMS = 0x704,
    TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET = 0x708,
    TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET = 0x70c,
    TEGRA_VIC_M_SET_FCE_UCODE_SIZE = 0x71c,
    TEGRA_VIC_M_OUTPUT_LUMA = 0x720,
    TEGRA_VIC_M_OUTPUT_CHROMA_U = 0x724,
    TEGRA_VIC_M_OUTPUT_CHROMA_V = 0x728,
    TEGRA_VIC_M_SET_FCE_UCODE_OFFSET = 0x72c,
};

#define TEGRA_VIC_EXECUTE_AWAKEN         (1u << 8)
#define TEGRA_VIC_CONTROL_GPTIMER_ON     (1u << 0)
#define TEGRA_VIC_CONTROL_FALCON_CONTROL (1u << 8)

// --- Configuration structure (VIC 4.0 ConfigStruct): byte offsets of its parts ------------------------------------
enum {
    TEGRA_VIC_CONFIG_BYTES = 0x610,
    TEGRA_VIC_PIPE = 0x000,            // PipeConfig, 16
    TEGRA_VIC_OUTPUT = 0x010,          // OutputConfig, 16
    TEGRA_VIC_OUTPUT_SURFACE = 0x020,  // OutputSurfaceConfig, 16
    TEGRA_VIC_OUT_MATRIX = 0x030,      // MatrixStruct outColorMatrixStruct, 32
    TEGRA_VIC_CLEAR_RECTS = 0x050,     // ClearRectStruct[4], 64
    TEGRA_VIC_SLOTS = 0x090,           // SlotStruct[8], 0xb0 each
    TEGRA_VIC_SLOT_BYTES = 0xb0,
    TEGRA_VIC_SLOT_CONFIG = 0x00,    // within a SlotStruct: SlotConfig, 64
    TEGRA_VIC_SLOT_SURFACE = 0x40,   // SlotSurfaceConfig, 16
    TEGRA_VIC_SLOT_LUMA_KEY = 0x50,  // LumaKeyStruct, 16
    TEGRA_VIC_SLOT_MATRIX = 0x60,    // MatrixStruct colorMatrixStruct, 32
    TEGRA_VIC_SLOT_GAMUT = 0x80,     // MatrixStruct gamutMatrixStruct, 32
    TEGRA_VIC_SLOT_BLEND = 0xa0,     // BlendingSlotStruct, 16
};

// Block kinds, cache widths, filter lengths, alpha fill modes (pixel formats: TegraVICFormats.h).
enum {
    TEGRA_VIC_BLK_PITCH = 0,
    TEGRA_VIC_BLK_16Bx2 = 1,  // NVB0B6_BLK_KIND_GENERIC_16Bx2: Tegra block linear, NVDEC's output layout
    TEGRA_VIC_CACHE_32Bx8 = 1,
    TEGRA_VIC_CACHE_64Bx4 = 2,
    TEGRA_VIC_FILTER_1TAP = 0,
    TEGRA_VIC_FILTER_2TAP = 1,
    TEGRA_VIC_ALPHA_OPAQUE = 0,
    TEGRA_VIC_ALPHA_BACKGROUND = 1,  // every pixel's alpha the background's (OUT_BACKGROUND_ALPHA)
    TEGRA_VIC_ALPHA_SOURCE_STREAM = 3,
};

// Stores `value` in bits [bit, bit + width) of the little-endian bit string at cfg + offset (a field never straddles
// more than two 64-bit words).
static inline void tegra_vic_put(uint8_t *cfg, uint32_t offset, uint32_t bit, uint32_t width, uint64_t value)
{
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t b = bit + i;
        uint8_t *p = cfg + offset + b / 8;
        if ((value >> i) & 1) {
            *p = (uint8_t)(*p | (1u << (b % 8)));
        } else {
            *p = (uint8_t)(*p & ~(1u << (b % 8)));
        }
    }
}

static inline uint64_t tegra_vic_get(const uint8_t *cfg, uint32_t offset, uint32_t bit, uint32_t width)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t b = bit + i;
        v |= (uint64_t)(((uint32_t)cfg[offset + b / 8] >> (b % 8)) & 1u) << i;
    }
    return v;
}

// Field positions (bit offset in its structure, width), from libdrm vic40.h.
#define TEGRA_VIC_F(name, bit, width) \
    enum {                            \
        name##_BIT = (bit),           \
        name##_WIDTH = (width)        \
    };
TEGRA_VIC_F(PIPE_DOWNSAMPLE_HORIZ, 0, 11)
TEGRA_VIC_F(PIPE_DOWNSAMPLE_VERT, 16, 11)
TEGRA_VIC_F(OUT_ALPHA_FILL_MODE, 0, 3)
TEGRA_VIC_F(OUT_ALPHA_FILL_SLOT, 3, 3)
TEGRA_VIC_F(OUT_BACKGROUND_ALPHA, 6, 10)
TEGRA_VIC_F(OUT_BACKGROUND_R, 16, 10)
TEGRA_VIC_F(OUT_BACKGROUND_G, 26, 10)
TEGRA_VIC_F(OUT_BACKGROUND_B, 36, 10)
TEGRA_VIC_F(OUT_FLIP_X, 48, 1)
TEGRA_VIC_F(OUT_FLIP_Y, 49, 1)
TEGRA_VIC_F(OUT_TRANSPOSE, 50, 1)
TEGRA_VIC_F(OUT_TARGET_LEFT, 64, 14)
TEGRA_VIC_F(OUT_TARGET_RIGHT, 80, 14)
TEGRA_VIC_F(OUT_TARGET_TOP, 96, 14)
TEGRA_VIC_F(OUT_TARGET_BOTTOM, 112, 14)
TEGRA_VIC_F(SURF_FORMAT, 0, 7)
TEGRA_VIC_F(SURF_CHROMA_LOC_HORIZ, 7, 2)
TEGRA_VIC_F(SURF_CHROMA_LOC_VERT, 9, 2)
TEGRA_VIC_F(SURF_BLK_KIND, 11, 4)
TEGRA_VIC_F(SURF_BLK_HEIGHT, 15, 4)
TEGRA_VIC_F(SURF_CACHE_WIDTH, 19, 3)  // slot surfaces only
TEGRA_VIC_F(SURF_WIDTH, 32, 14)
TEGRA_VIC_F(SURF_HEIGHT, 46, 14)
TEGRA_VIC_F(SURF_LUMA_WIDTH, 64, 14)
TEGRA_VIC_F(SURF_LUMA_HEIGHT, 78, 14)
TEGRA_VIC_F(SURF_CHROMA_WIDTH, 96, 14)
TEGRA_VIC_F(SURF_CHROMA_HEIGHT, 110, 14)
TEGRA_VIC_F(SLOT_ENABLE, 0, 1)
TEGRA_VIC_F(SLOT_CURRENT_FIELD, 8, 1)
TEGRA_VIC_F(SLOT_FRAME_FORMAT, 16, 4)
TEGRA_VIC_F(SLOT_FILTER_Y, 20, 2)
TEGRA_VIC_F(SLOT_FILTER_X, 22, 2)
TEGRA_VIC_F(SLOT_SOFT_CLAMP_LOW, 128, 10)
TEGRA_VIC_F(SLOT_SOFT_CLAMP_HIGH, 138, 10)
TEGRA_VIC_F(SLOT_PLANAR_ALPHA, 160, 10)
TEGRA_VIC_F(SLOT_CONSTANT_ALPHA, 170, 1)
TEGRA_VIC_F(SLOT_SOURCE_LEFT, 256, 30)
TEGRA_VIC_F(SLOT_SOURCE_RIGHT, 288, 30)
TEGRA_VIC_F(SLOT_SOURCE_TOP, 320, 30)
TEGRA_VIC_F(SLOT_SOURCE_BOTTOM, 352, 30)
TEGRA_VIC_F(SLOT_DEST_LEFT, 384, 14)
TEGRA_VIC_F(SLOT_DEST_RIGHT, 400, 14)
TEGRA_VIC_F(SLOT_DEST_TOP, 416, 14)
TEGRA_VIC_F(SLOT_DEST_BOTTOM, 432, 14)
TEGRA_VIC_F(MATRIX_C00, 0, 20)
TEGRA_VIC_F(MATRIX_C10, 20, 20)
TEGRA_VIC_F(MATRIX_C20, 40, 20)
TEGRA_VIC_F(MATRIX_SHIFT, 60, 4)
TEGRA_VIC_F(MATRIX_C01, 64, 20)
TEGRA_VIC_F(MATRIX_C11, 84, 20)
TEGRA_VIC_F(MATRIX_C21, 104, 20)
TEGRA_VIC_F(MATRIX_ENABLE, 127, 1)
TEGRA_VIC_F(MATRIX_C02, 128, 20)
TEGRA_VIC_F(MATRIX_C12, 148, 20)
TEGRA_VIC_F(MATRIX_C22, 168, 20)
TEGRA_VIC_F(MATRIX_C03, 192, 20)
TEGRA_VIC_F(MATRIX_C13, 212, 20)
TEGRA_VIC_F(MATRIX_C23, 232, 20)
#undef TEGRA_VIC_F
#define TEGRA_VIC_SET(cfg, base, name, value) tegra_vic_put((cfg), (base), name##_BIT, name##_WIDTH, (uint64_t)(value))
#define TEGRA_VIC_GET(cfg, base, name)        tegra_vic_get((cfg), (base), name##_BIT, name##_WIDTH)

// --- Colour matrices ----------------------------------------------------------------------------------------------
// A 3x4 matrix in the engine's encoding: out[r] = (((c[r][0] * in0 + c[r][1] * in1 + c[r][2] * in2) >> shift)
// + c[r][3]) >> 8, clamped to 0..1023, on 10-bit pipeline values (in10 = in8 << 2, out8 = out10 >> 2). So a gain g
// is round(g * 2^(shift + 8)) and an offset of o 10-bit steps is round(o * 256). Coefficients are 20-bit signed.
typedef struct {
    int32_t c[3][4];
    uint32_t shift;
} TegraVICMatrix;

#define TEGRA_VIC_MATRIX_SHIFT 8u  // 1.0 = 65536: every gain below 8 fits the 20 bits

static inline void tegra_vic_matrix_put(uint8_t *cfg, uint32_t base, const TegraVICMatrix *m)
{
    const uint32_t mask = (1u << 20) - 1;
    TEGRA_VIC_SET(cfg, base, MATRIX_C00, (uint32_t)m->c[0][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C01, (uint32_t)m->c[0][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C02, (uint32_t)m->c[0][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C03, (uint32_t)m->c[0][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C10, (uint32_t)m->c[1][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C11, (uint32_t)m->c[1][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C12, (uint32_t)m->c[1][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C13, (uint32_t)m->c[1][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C20, (uint32_t)m->c[2][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C21, (uint32_t)m->c[2][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C22, (uint32_t)m->c[2][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C23, (uint32_t)m->c[2][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_SHIFT, m->shift);
    TEGRA_VIC_SET(cfg, base, MATRIX_ENABLE, 1);
}

static inline uint32_t tegra_vic_clamp(int64_t v, uint32_t max)
{
    if (v < 0)
        return 0;
    return v > max ? max : (uint32_t)v;
}

// The engine's arithmetic for one pixel (the law above), for the host test's expectations. Both
// stages shift the signed sum arithmetically: the engine floors negative sums, it does not round them toward zero.
static inline void tegra_vic_matrix_apply(const TegraVICMatrix *m, const uint32_t in10[3], uint32_t out10[3])
{
    for (int r = 0; r < 3; r++) {
        int64_t acc = (int64_t)m->c[r][0] * in10[0] + (int64_t)m->c[r][1] * in10[1] + (int64_t)m->c[r][2] * in10[2];
        acc = (acc >> m->shift) + m->c[r][3];  // NOLINT(bugprone-signed-bitwise): arithmetic shift, see above
        acc >>= 8;                             // NOLINT(bugprone-signed-bitwise): arithmetic shift, see above
        out10[r] = tegra_vic_clamp(acc, 1023);
    }
}

// Fixed-point rounding of a gain or offset given in millionths (kernel code has no floating point).
static inline int32_t tegra_vic_fix(int64_t micros, int64_t one)
{
    const int64_t n = micros * one;
    return (int32_t)(n >= 0 ? (n + 500000) / 1000000 : -((-n + 500000) / 1000000));
}

enum {
    TEGRA_VIC_YCC_601 = 0,
    TEGRA_VIC_YCC_709 = 1,
    TEGRA_VIC_YCC_2020 = 2
};

// Non-constant-luminance luma weights (Kr, Kb), in millionths.
static inline void tegra_vic_kr_kb(uint32_t standard, int64_t *kr, int64_t *kb)
{
    *kr = standard == TEGRA_VIC_YCC_2020 ? 262700 : standard == TEGRA_VIC_YCC_709 ? 212600 : 299000;
    *kb = standard == TEGRA_VIC_YCC_2020 ? 59300 : standard == TEGRA_VIC_YCC_709 ? 72200 : 114000;
}

// Which pipeline channel carries Cb and which Cr for the 4:2:0 formats (ch0 is always Y). The unit's self-test found
// it; switch-frame-tap measured Cr in channel 1 and Cb in channel 2 for T_Y8___U8V8_N420 with NV12 data.
typedef struct {
    uint32_t cbChannel, crChannel;
} TegraVICChroma;

// RGB (pipeline channels R, G, B) -> Y'CbCr, 8-bit video range (Y 16..235, C 16..240) or full range (Y 0..255, C 128
// +- 127.5), rounded to nearest at the final >> 2 (offset + 2 10-bit steps).
static inline void tegra_vic_matrix_rgb_to_ycc(uint32_t standard, bool full, const TegraVICChroma *ch,
                                               TegraVICMatrix *m)
{
    int64_t kr = 0;
    int64_t kb = 0;
    tegra_vic_kr_kb(standard, &kr, &kb);
    const int64_t kg = 1000000 - kr - kb;
    const int64_t ys = full ? 1000000 : 1000000 * 219 / 255;
    const int64_t cs = full ? 1000000 : 1000000 * 224 / 255;
    const int64_t one = (int64_t)(1ULL << (TEGRA_VIC_MATRIX_SHIFT + 8));
    // Rows in micro-units: Y = kr R + kg G + kb B; Cb = (B - Y) / (2 (1 - kb)); Cr = (R - Y) / (2 (1 - kr)).
    const int64_t y[3] = {kr * ys / 1000000, kg * ys / 1000000, kb * ys / 1000000};
    const int64_t cbDen = 2 * (1000000 - kb);
    const int64_t crDen = 2 * (1000000 - kr);
    const int64_t cb[3] = {-kr * cs / cbDen, -kg * cs / cbDen, (1000000 - kb) * cs / cbDen};
    const int64_t cr[3] = {(1000000 - kr) * cs / crDen, -kg * cs / crDen, -kb * cs / crDen};
    memset(m, 0, sizeof(*m));
    m->shift = TEGRA_VIC_MATRIX_SHIFT;
    for (int i = 0; i < 3; i++) {
        m->c[0][i] = tegra_vic_fix(y[i], one);
        m->c[ch->cbChannel][i] = tegra_vic_fix(cb[i], one);
        m->c[ch->crChannel][i] = tegra_vic_fix(cr[i], one);
    }
    // Offsets in 10-bit steps (x256), +2 steps so out8 = out10 >> 2 rounds: Y 64 (video) or 0; C 512.
    m->c[0][3] = (full ? 0 : 64 * 256) + 512;
    m->c[ch->cbChannel][3] = 512 * 256 + 512;
    m->c[ch->crChannel][3] = 512 * 256 + 512;
}

// Y'CbCr -> RGB, the inverse: R = Y + 2(1-kr) Cr; B = Y + 2(1-kb) Cb; G = Y - (2kb(1-kb) Cb + 2kr(1-kr) Cr) / kg.
static inline void tegra_vic_matrix_ycc_to_rgb(uint32_t standard, bool full, const TegraVICChroma *ch,
                                               TegraVICMatrix *m)
{
    int64_t kr = 0;
    int64_t kb = 0;
    tegra_vic_kr_kb(standard, &kr, &kb);
    const int64_t kg = 1000000 - kr - kb;
    // Gains of the 8-bit code values: luma 255/219 (video) or 1; chroma 255/224 (video) or 1.
    const int64_t gy = full ? 1000000 : 1000000 * 255 / 219;
    const int64_t gc = full ? 1000000 : 1000000 * 255 / 224;
    const int64_t rCr = 2 * (1000000 - kr) * gc / 1000000;
    const int64_t bCb = 2 * (1000000 - kb) * gc / 1000000;
    const int64_t gCb = 2 * kb * (1000000 - kb) / kg * gc / 1000000;
    const int64_t gCr = 2 * kr * (1000000 - kr) / kg * gc / 1000000;
    const int64_t one = (int64_t)(1ULL << (TEGRA_VIC_MATRIX_SHIFT + 8));
    memset(m, 0, sizeof(*m));
    m->shift = TEGRA_VIC_MATRIX_SHIFT;
    const uint32_t y = 0;
    const uint32_t cb = ch->cbChannel;
    const uint32_t cr = ch->crChannel;
    m->c[0][y] = m->c[1][y] = m->c[2][y] = tegra_vic_fix(gy, one);
    m->c[0][cr] = tegra_vic_fix(rCr, one);
    m->c[1][cb] = tegra_vic_fix(-gCb, one);
    m->c[1][cr] = tegra_vic_fix(-gCr, one);
    m->c[2][cb] = tegra_vic_fix(bCb, one);
    // Offsets: -(gain x black level) in 10-bit steps (x256): luma 64 (video), chroma 512; +2 steps to round.
    const int64_t y0 = full ? 0 : 64;
    m->c[0][3] = tegra_vic_fix(-(gy * y0 + rCr * 512), 256) + 512;
    m->c[1][3] = tegra_vic_fix(-(gy * y0 - gCb * 512 - gCr * 512), 256) + 512;
    m->c[2][3] = tegra_vic_fix(-(gy * y0 + bCb * 512), 256) + 512;
}

// The full-range BT.709 RGB of an 8-bit Y'CbCr sample: the float conversion in integer millionths, rounded and clamped
// (the reference for reading 4:2:0; the host test checks it against the float formula).
static inline void tegra_vic_ideal_rgb709_full(const uint32_t ycc[3], uint32_t rgb[3])
{
    const int64_t y = (int64_t)ycc[0] * 1000000;
    const int64_t cb = (int64_t)ycc[1] - 128;
    const int64_t cr = (int64_t)ycc[2] - 128;
    const int64_t v[3] = {y + 1574800 * cr, y - 187324 * cb - 468124 * cr, y + 1855600 * cb};
    for (int c = 0; c < 3; c++) {
        const int64_t r = v[c] >= 0 ? (v[c] + 500000) / 1000000 : -((-v[c] + 500000) / 1000000);
        rgb[c] = tegra_vic_clamp(r, 255);
    }
}

// --- Jobs ---------------------------------------------------------------------------------------------------------
// One surface as the engine sees it: plane addresses in VIC's (SMMU) address space and how far each may extend.
typedef struct {
    const TegraVICFormat *format;
    uint32_t width, height;                // pixels
    uint32_t pitch[TEGRA_VIC_MAX_PLANES];  // bytes per row of each plane
    uint32_t rows[TEGRA_VIC_MAX_PLANES];   // rows each plane holds
    uint64_t base[TEGRA_VIC_MAX_PLANES];   // plane addresses
    uint64_t span[TEGRA_VIC_MAX_PLANES];   // bytes mapped from base[p] (the checker keeps every access inside)
    uint32_t standard;                     // TEGRA_VIC_YCC_* for the 4:2:0 formats
    uint32_t transfer, primaries;
    uint32_t chromaSite;  // IOSurfaceAccelerator siting code 0..6 (0: unspecified)
    // Tegra block linear (16Bx2 sectors in 64Bx8 GOBs, 2^blockHeightLog2 GOBs a block: NVDEC writes blockHeightLog2
    // 1), as averne's FFmpeg nvtegra programs it on either side (SlotBlkKind, OutBlkKind GENERIC_16Bx2): pitch a whole
    // number of GOBs, each plane's rows whole blocks, `rows` the plane's aligned height (the engine reads and writes
    // whole blocks).
    uint32_t blockLinear, blockHeightLog2;
    // A destination that is a rectangle of a larger surface: its last row ends at its width, and the bytes after it
    // (another rectangle's) are not mapped. VIC writes only the surface's width of each row.
    uint32_t lastRowExact;
} TegraVICImage;

// Reading 4:2:0 (TegraVICTraits.yccIn): to RGB through the slot matrix (within a few steps of the float conversion),
// and the same-format copy that passes the channels through. Without the bit those jobs are refused (callers fall back).
enum {
    TEGRA_VIC_YCC_IN_RGB = 1,
    TEGRA_VIC_YCC_IN_COPY = 2
};

// IOSurfaceAccelerator's transform bits (the request's Transform option, flags bits 0..2).
enum {
    TEGRA_VIC_FLIP_H = 1,
    TEGRA_VIC_FLIP_V = 2,
    TEGRA_VIC_ROT90 = 4
};

typedef struct {
    uint64_t srcX, srcY, srcW, srcH;  // source rectangle, 16.16 fixed
    uint32_t dstX, dstY, dstW, dstH;  // destination rectangle, pixels
    uint32_t transform;               // TEGRA_VIC_FLIP_* | TEGRA_VIC_ROT90
    uint32_t nearest;                 // 1-tap filter instead of 2-tap
    uint32_t fill;                    // no source: write the colour below over the rectangle
    uint32_t fill10[3];               // the fill's R, G, B (10-bit; converted for a Y'CbCr destination)
    uint32_t fillAlpha10;             // and its alpha (10-bit)
} TegraVICOp;

// What this engine does with the configurations tegra_vic_build makes.
typedef struct {
    TegraVICChroma in, out;        // channel of Cb/Cr when reading and writing 4:2:0
    uint8_t vicFlips[8][3];        // IOSurfaceAccelerator transform -> OutputFlipX, OutputFlipY, OutputTranspose
    uint32_t preTransposeRects;    // 1: a transposed job's target/destination rectangles are given before the transpose
    uint32_t preTransposeSurface;  // 1: and its whole output surface (size, luma and chroma memory geometry): measured
                                   // apart from the rectangles
    uint32_t rotations;            // bit t: transform t proven (a whole-surface copy lands as that transform)
    uint32_t bilinear;             // 2-tap filtering proven (else every job is 1-tap)
    uint32_t sourcePitchAlign;     // a source row pitch VIC reads exactly is a multiple of this (bytes)
    uint32_t destinationPitchAlign;  // and a destination row pitch it writes exactly
    uint32_t fillThroughMatrix;  // 1: the background colour goes through the output matrix (else it is converted here)
    uint32_t yccIn;              // what reading a 4:2:0 source was proven to do: TEGRA_VIC_YCC_IN_*
    uint32_t blockLinearIn;      // 1: a block-linear 4:2:0 source (NVDEC's layout) copies exactly into pitch linear
    uint32_t blockLinearOut;     // 1: an untransposed destination may be block linear (OutBlkKind 16Bx2)
    uint32_t packedYcc;          // 1: packed 4:2:2 ('2vuy', 'yuvs') reads and writes one element a pixel
    uint32_t subRects;           // bit t: under transform t a destination rectangle inside a larger surface lands in
                                 // place (where the flips mirror is measured, not assumed); transform 0 always
    uint32_t surfaceFlips;       // measured flips precede transpose and mirror around the complete output surface
    uint32_t multiSlot;          // 1: one job composes several slots, bottom first, each over those below
                                 // (TegraVICCompose.h)
    uint32_t slotAlpha;          // 1: a slot blends by its pixels' alpha or a global alpha, not only covers
} TegraVICTraits;

// VIC 4.0 as the unit measured it with a boot self-test on all 19 boots from 11-05 23:53 to 11-06 23:42, every line
// the same: Cb in channel 2 both ways, 4:2:0 read to RGB within two steps (samples widened x << 2 | x >> 6) and
// passed through exactly, the background colour through the output matrix, 2-tap scaling, rows on 256 bytes (144- and
// 192-byte pitches lost half the copy), all 8 transforms with a transposed job's rectangles and surface given before
// the transpose, and unadjusted sub-rectangles in place under transforms 0 and 6 only (flips mirror around the surface).
// The flip table is the transform probe's on test/vic_model.h, whose output reproduces every one of those boots'
// transpose and sub-rectangle diag lines (tegra_vic_transpose_test pins both).
static const TegraVICTraits kTegraVICTraits = {
    .in = {2, 1},
    .out = {2, 1},
    .vicFlips = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 1, 1}, {1, 1, 1}, {0, 0, 1}, {1, 0, 1}},
    .preTransposeRects = 1,
    .preTransposeSurface = 1,
    .rotations = 0xff,
    .bilinear = 1,
    // Probe gpu.x1.vic_pitch (LIMITS V9): rows of 320, 576 and 2880 bytes (multiples of 64, none of 128) are read as if
    // rounded down to 256 bytes as a source and written exactly as a destination.
    .sourcePitchAlign = 256,
    .destinationPitchAlign = 64,
    .fillThroughMatrix = 1,
    .yccIn = TEGRA_VIC_YCC_IN_RGB | TEGRA_VIC_YCC_IN_COPY,
    .blockLinearIn = 1,
    .blockLinearOut = 1,  // averne's nvtegra writes T210 VIC output in GENERIC_16Bx2: probe gpu.x1.vic_block_linear
    .packedYcc = 0,       // a packed 4:2:2 row's element size and byte order: probe gpu.x1.vic_packed422
    .subRects = 0x41,
    .surfaceFlips = 1,
    .multiSlot = 1,  // no job has run two slots yet: probe gpu.x1.vic_compose
    .slotAlpha = 1,  // the same probe's alpha cases, apart so an alpha failure leaves opaque composites on
};

#define TEGRA_VIC_MAX_METHODS 12

typedef struct {
    uint8_t config[TEGRA_VIC_CONFIG_BYTES];
    uint32_t method[TEGRA_VIC_MAX_METHODS], value[TEGRA_VIC_MAX_METHODS];
    uint32_t count;
    uint64_t pixels;  // read plus written: the engine's wait bound scales with it
} TegraVICJob;

// Limits: VIC's rectangles are 14-bit; IOSurfaceAcceleratorCapabilitiesDict publishes these.
enum {
    TEGRA_VIC_MAX_DIM = 8192,
    TEGRA_VIC_MIN_DIM = 2,
    TEGRA_VIC_MAX_UPSCALE = 16,    // destination / source, per axis
    TEGRA_VIC_MAX_DOWNSCALE = 16,  // source / destination, per axis
};

static inline uint32_t tegra_vic_pitch_align(const TegraVICTraits *t, bool source)
{
    return source ? t->sourcePitchAlign : t->destinationPitchAlign;
}

// Validates an image: geometry the engine can address and every plane's extent inside its mapping.
static inline const char *tegra_vic_check_image(const TegraVICImage *im, uint32_t pitchAlign)
{
    const TegraVICFormat *f = im->format;
    if (!f)
        return "pixel format the scaler does not take";
    if (im->width < TEGRA_VIC_MIN_DIM || im->height < TEGRA_VIC_MIN_DIM || im->width > TEGRA_VIC_MAX_DIM ||
        im->height > TEGRA_VIC_MAX_DIM)
        return "surface size outside 2..8192";
    if (f->subY && ((im->width | im->height) & 1))
        return "4:2:0 surface with an odd size";
    if (f->subX && (im->width & 1))
        return "4:2:2 surface of odd width";
    if (im->blockLinear && im->blockHeightLog2 > 5)
        return "block height VIC does not address";
    // VIC has one ChromaWidth/ChromaHeight for both chroma planes of a three-plane surface.
    if (f->planes > 2 && (im->pitch[2] != im->pitch[1] || im->rows[2] != im->rows[1]))
        return "chroma planes of different geometry";
    for (uint32_t p = 0; p < f->planes; p++) {
        const uint32_t w = tegra_vic_plane_width(f, im->width, p);
        const uint32_t h = tegra_vic_plane_rows(f, im->height, p);
        uint32_t align = pitchAlign;
        if (im->blockLinear)
            align = 64;  // GOBs are 64 wide
        if (im->base[p] & 0xff)
            return "plane not 256-byte aligned";
        if (im->pitch[p] % align || im->pitch[p] % f->bpe[p])
            return "row pitch the engine cannot address";
        if (im->blockLinear && im->rows[p] % (8u << im->blockHeightLog2))
            return "block-linear plane of partial blocks";
        if ((uint64_t)w * f->bpe[p] > im->pitch[p])
            return "row pitch shorter than a row";
        if (im->pitch[p] / f->bpe[p] > 16384)
            return "row pitch over 16384 elements";
        if (im->rows[p] < h)
            return "plane shorter than the surface";
        // The engine reads or writes (LumaHeight + 1) rows of (LumaWidth + 1) elements: h rows of pitch bytes (a
        // block-linear plane: all its rows, whole blocks).
        uint64_t extent = (uint64_t)im->pitch[p] * h;
        if (im->blockLinear) {
            extent = (uint64_t)im->pitch[p] * im->rows[p];
        } else if (im->lastRowExact) {
            extent = (uint64_t)im->pitch[p] * (h - 1) + (uint64_t)w * f->bpe[p];
        }
        if (extent > im->span[p])
            return "plane extends past its mapping";
    }
    return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
}

// Builds the configuration and methods for one job. `configAddr`, `filterAddr` are where the configuration and the
// filter structure sit in VIC's address space. Returns 0, or why the job is refused (nothing is written then).
static inline const char *tegra_vic_build(const TegraVICImage *src, const TegraVICImage *dst, const TegraVICOp *op,
                                          const TegraVICTraits *t, uint64_t configAddr, uint64_t filterAddr,
                                          TegraVICJob *job)
{
    if (dst->blockLinear && !t->blockLinearOut)
        return "block-linear destination this engine has not proven";
    if ((dst->format && dst->format->packed && !t->packedYcc) ||
        (!op->fill && src->format && src->format->packed && !t->packedYcc))
        return "packed 4:2:2 this engine has not proven";
    const char *why = tegra_vic_check_image(dst, tegra_vic_pitch_align(t, false));
    if (!why && !op->fill)
        why = tegra_vic_check_image(src, tegra_vic_pitch_align(t, true));
    if (why)
        return why;
    if (op->transform > 7)
        return "transform outside the 8 flips and rotations";
    if (!(t->rotations & (1u << op->transform)))
        return "transform this engine has not proven";
    if (!op->dstW || !op->dstH || op->dstX + (uint64_t)op->dstW > dst->width ||
        op->dstY + (uint64_t)op->dstH > dst->height)
        return "destination rectangle outside the destination";
    if ((configAddr | filterAddr) & 0xff)
        return "configuration not 256-byte aligned";
    const int transpose = t->vicFlips[op->transform][2];
    if (!op->fill) {
        if (op->srcW < (1u << 16) || op->srcH < (1u << 16))
            return "source rectangle under one pixel";
        if (op->srcX + op->srcW > ((uint64_t)src->width << 16) || op->srcY + op->srcH > ((uint64_t)src->height << 16))
            return "source rectangle outside the source";
        // Scale per output axis: a transposed job's destination width comes from the source height.
        const uint64_t sw = transpose ? op->srcH : op->srcW;
        const uint64_t sh = transpose ? op->srcW : op->srcH;
        if (((uint64_t)op->dstW << 16) > sw * TEGRA_VIC_MAX_UPSCALE ||
            ((uint64_t)op->dstH << 16) > sh * TEGRA_VIC_MAX_UPSCALE)
            return "upscale past 16x";
        if (sw > ((uint64_t)op->dstW << 16) * TEGRA_VIC_MAX_DOWNSCALE ||
            sh > ((uint64_t)op->dstH << 16) * TEGRA_VIC_MAX_DOWNSCALE)
            return "downscale past 16x";
    }
    if (dst->format->subY && ((op->dstX | op->dstY | op->dstW | op->dstH) & 1))
        return "4:2:0 destination rectangle on odd coordinates";
    if (dst->format->subX && ((op->dstX | op->dstW) & 1))
        return "4:2:2 destination rectangle on odd columns";
    if (!op->fill && op->transform && !t->surfaceFlips && !(t->subRects & (1u << op->transform)) &&
        (op->dstX || op->dstY || op->dstW != dst->width || op->dstH != dst->height))
        return "destination sub-rectangle this transform has not proven";
    // NVDEC's pictures, the only block-linear sources the unit has run, are 4:2:0. An L8 source is such a picture's
    // luma plane alone, the same GOBs (a 'v0a8' alpha plane: HW Test decode.hevc_alpha).
    if (!op->fill && src->blockLinear &&
        (!t->blockLinearIn || !(src->format->subY || src->format->vic == TEGRA_VIC_T_L8)))
        return "block-linear source this engine has not proven";
    if (!op->fill && src->format->kind != TEGRA_VIC_KIND_RGB) {
        const bool same = dst->format->kind == src->format->kind && src->standard == dst->standard &&
                          t->in.cbChannel == t->out.cbChannel;  // the pass-through tegra_vic_build programs below
        if (!(t->yccIn & (same ? TEGRA_VIC_YCC_IN_COPY : TEGRA_VIC_YCC_IN_RGB)))
            return same ? "4:2:0 copy this engine has not proven" : "4:2:0 conversion this engine has not proven";
    }

    const bool copyYcc = !op->fill && src->format->kind != TEGRA_VIC_KIND_RGB &&
                         src->format->kind == dst->format->kind && src->standard == dst->standard &&
                         t->in.cbChannel == t->out.cbChannel;
    if (!copyYcc && ((dst->format->kind != TEGRA_VIC_KIND_RGB && dst->standard > TEGRA_VIC_YCC_2020) ||
                     (!op->fill && src->format->kind != TEGRA_VIC_KIND_RGB && src->standard > TEGRA_VIC_YCC_2020)))
        return "YCbCr matrix requires color conversion outside VIC's contract";

    if (!op->fill && ((src->transfer && dst->transfer && src->transfer != dst->transfer) ||
                      (src->primaries && dst->primaries && src->primaries != dst->primaries)))
        return "transfer or gamut conversion requires a color shader";
    if (!op->fill && ((src->transfer && !dst->transfer) || (src->primaries && !dst->primaries)))
        return "explicit source encoding requires destination metadata";

    memset(job, 0, sizeof(*job));
    uint8_t *c = job->config;
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_HORIZ, 4);  // 1.0, U9.2
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_VERT, 4);

    // Output. With the rectangles (and, apart, the output surface) given before the transpose (traits the unit
    // proves), they are swapped for a transposed job. The surface before the transpose is all of OutputSurfaceConfig
    // (hekate's programming): its size, and the planes' memory geometry with width and height exchanged, since the
    // engine writes composed pixel (x, y) to memory row x, column y (LumaHeight + 1 elements a row, LumaWidth + 1 rows).
    const bool swap = transpose != 0 && t->preTransposeRects != 0;
    const bool swapSurface = transpose != 0 && t->preTransposeSurface != 0;
    if (swapSurface && (!dst->format->vicTransposed || dst->blockLinear))
        return dst->blockLinear ? "transposed block-linear destination this engine has not proven"
                                : "transposed output this format has no layout for";
    const uint32_t ow = swapSurface ? dst->height : dst->width;
    const uint32_t oh = swapSurface ? dst->width : dst->height;
    const uint32_t tw = swap ? op->dstH : op->dstW;
    const uint32_t th = swap ? op->dstW : op->dstH;
    uint32_t tx = swap ? op->dstY : op->dstX;
    uint32_t ty = swap ? op->dstX : op->dstY;
    if (t->surfaceFlips) {
        if (t->vicFlips[op->transform][0])
            tx = ow - tx - tw;
        if (t->vicFlips[op->transform][1])
            ty = oh - ty - th;
    }
    const bool alpha = op->fill == 0 && src->format->alpha != 0 && dst->format->alpha != 0;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE,
                  alpha ? TEGRA_VIC_ALPHA_SOURCE_STREAM : TEGRA_VIC_ALPHA_OPAQUE);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA, op->fill ? op->fillAlpha10 & 1023 : 1023);
    if (op->fill) {
        // A Y'CbCr destination: the background goes through the output matrix if the engine does that (a trait the
        // unit measured), else it is converted here, through the same arithmetic, into the output channels.
        uint32_t bg[3] = {op->fill10[0] & 1023, op->fill10[1] & 1023, op->fill10[2] & 1023};
        if (dst->format->kind != TEGRA_VIC_KIND_RGB) {
            TegraVICMatrix m;
            tegra_vic_matrix_rgb_to_ycc(dst->standard, dst->format->kind == TEGRA_VIC_KIND_YCC_FULL, &t->out, &m);
            if (t->fillThroughMatrix) {
                tegra_vic_matrix_put(c, TEGRA_VIC_OUT_MATRIX, &m);
            } else {
                const uint32_t rgb[3] = {bg[0], bg[1], bg[2]};
                tegra_vic_matrix_apply(&m, rgb, bg);
            }
        }
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R, bg[0]);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G, bg[1]);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B, bg[2]);
    }
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X, t->vicFlips[op->transform][0]);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_Y, t->vicFlips[op->transform][1]);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE, transpose);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT, tx);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT, tx + tw - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP, ty);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM, ty + th - 1);

    // Chroma halved across memory rows is, before the transpose, chroma halved down: a transposed 4:2:2 output is
    // described as N422R, its siting exchanged too.
    const TegraVICFormat *df = dst->format;
    uint32_t locH = 0;
    uint32_t locV = 0;
    tegra_vic_chroma_loc(df, dst->chromaSite, swapSurface, &locH, &locV);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT, swapSurface ? df->vicTransposed : df->vic);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ, locH);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT, locV);
    const bool blOut = dst->blockLinear != 0;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND, blOut ? TEGRA_VIC_BLK_16Bx2 : TEGRA_VIC_BLK_PITCH);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_HEIGHT, blOut ? dst->blockHeightLog2 : 0);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH, ow - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT, oh - 1);
    const uint32_t lumaRow = dst->pitch[0] / df->bpe[0];
    const uint32_t lumaRows = blOut ? dst->rows[0] : dst->height;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH, (swapSurface ? lumaRows : lumaRow) - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT, (swapSurface ? lumaRow : lumaRows) - 1);
    if (df->planes > 1) {
        const uint32_t chromaRow = dst->pitch[1] / df->bpe[1];
        const uint32_t chromaRows = blOut ? dst->rows[1] : tegra_vic_plane_rows(df, dst->height, 1);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH, (swapSurface ? chromaRows : chromaRow) - 1);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT, (swapSurface ? chromaRow : chromaRows) - 1);
    } else {
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH, 16383);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT, 16383);
    }

    const bool dstYcc = df->kind != TEGRA_VIC_KIND_RGB;
    if (!op->fill) {
        const TegraVICFormat *sf = src->format;
        const bool srcYcc = sf->kind != TEGRA_VIC_KIND_RGB;
        const uint32_t slot = TEGRA_VIC_SLOTS;  // slot 0
        uint8_t *s = c;
        TEGRA_VIC_SET(s, slot, SLOT_ENABLE, 1);
        TEGRA_VIC_SET(s, slot, SLOT_CURRENT_FIELD, 1);
        const uint32_t filter = op->nearest || !t->bilinear ? TEGRA_VIC_FILTER_1TAP : TEGRA_VIC_FILTER_2TAP;
        TEGRA_VIC_SET(s, slot, SLOT_FILTER_X, filter);
        TEGRA_VIC_SET(s, slot, SLOT_FILTER_Y, filter);
        TEGRA_VIC_SET(s, slot, SLOT_SOFT_CLAMP_LOW, 0);
        TEGRA_VIC_SET(s, slot, SLOT_SOFT_CLAMP_HIGH, 1023);
        TEGRA_VIC_SET(s, slot, SLOT_PLANAR_ALPHA, 1023);
        TEGRA_VIC_SET(s, slot, SLOT_CONSTANT_ALPHA, 1);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_LEFT, op->srcX);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_RIGHT, op->srcX + op->srcW - (1u << 16));
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_TOP, op->srcY);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_BOTTOM, op->srcY + op->srcH - (1u << 16));
        TEGRA_VIC_SET(s, slot, SLOT_DEST_LEFT, tx);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_RIGHT, tx + tw - 1);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_TOP, ty);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_BOTTOM, ty + th - 1);
        const uint32_t surf = slot + TEGRA_VIC_SLOT_SURFACE;
        uint32_t inH = 0;
        uint32_t inV = 0;
        tegra_vic_chroma_loc(sf, src->chromaSite, 0, &inH, &inV);
        TEGRA_VIC_SET(s, surf, SURF_FORMAT, sf->vic);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_LOC_HORIZ, inH);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_LOC_VERT, inV);
        // Block linear (averne's nvtegra VIC setup for NVDEC's pictures): 16Bx2 kind, the GOBs a block, the 32Bx8
        // cache, and the planes' heights as their whole blocks.
        const bool bl = src->blockLinear != 0;
        TEGRA_VIC_SET(s, surf, SURF_BLK_KIND, bl ? TEGRA_VIC_BLK_16Bx2 : TEGRA_VIC_BLK_PITCH);
        TEGRA_VIC_SET(s, surf, SURF_BLK_HEIGHT, bl ? src->blockHeightLog2 : 0);
        TEGRA_VIC_SET(s, surf, SURF_CACHE_WIDTH, bl ? TEGRA_VIC_CACHE_32Bx8 : TEGRA_VIC_CACHE_64Bx4);
        TEGRA_VIC_SET(s, surf, SURF_WIDTH, src->width - 1);
        TEGRA_VIC_SET(s, surf, SURF_HEIGHT, src->height - 1);
        TEGRA_VIC_SET(s, surf, SURF_LUMA_WIDTH, src->pitch[0] / sf->bpe[0] - 1);
        TEGRA_VIC_SET(s, surf, SURF_LUMA_HEIGHT, (bl ? src->rows[0] : src->height) - 1);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_WIDTH, sf->planes > 1 ? src->pitch[1] / sf->bpe[1] - 1 : 16383);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_HEIGHT,
                      sf->planes > 1 ? (bl ? src->rows[1] : tegra_vic_plane_rows(sf, src->height, 1)) - 1 : 16383);

        // Colour: Y'CbCr in -> RGB through the slot matrix, RGB -> Y'CbCr out through the output matrix. A Y'CbCr to
        // Y'CbCr job of the same range and standard passes the channels through untouched (the identity, measured on
        // 4:2:0), whatever the subsampling on either side.
        TegraVICMatrix m;
        const bool sameYcc = srcYcc && dstYcc && sf->kind == df->kind && src->standard == dst->standard &&
                             t->in.cbChannel == t->out.cbChannel;
        if (srcYcc && !sameYcc) {
            tegra_vic_matrix_ycc_to_rgb(src->standard, sf->kind == TEGRA_VIC_KIND_YCC_FULL, &t->in, &m);
            tegra_vic_matrix_put(s, slot + TEGRA_VIC_SLOT_MATRIX, &m);
        }
        if (dstYcc && !sameYcc) {
            tegra_vic_matrix_rgb_to_ycc(dst->standard, df->kind == TEGRA_VIC_KIND_YCC_FULL, &t->out, &m);
            tegra_vic_matrix_put(c, TEGRA_VIC_OUT_MATRIX, &m);
        }
    }

    uint32_t n = 0;
#define TEGRA_VIC_METHOD(m, v)         \
    do {                               \
        job->method[n] = (m);          \
        job->value[n] = (uint32_t)(v); \
        n++;                           \
    } while (0)
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_APPLICATION_ID, 1);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_CONTROL_PARAMS, (TEGRA_VIC_CONFIG_BYTES / 16u) << 16 |
                                                         TEGRA_VIC_CONTROL_GPTIMER_ON |
                                                         TEGRA_VIC_CONTROL_FALCON_CONTROL);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET, configAddr >> 8);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET, filterAddr >> 8);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_OUTPUT_LUMA, dst->base[0] >> 8);
    if (df->planes > 1)
        TEGRA_VIC_METHOD(TEGRA_VIC_M_OUTPUT_CHROMA_U, dst->base[1] >> 8);
    if (df->planes > 2)
        TEGRA_VIC_METHOD(TEGRA_VIC_M_OUTPUT_CHROMA_V, dst->base[2] >> 8);
    if (!op->fill) {
        TEGRA_VIC_METHOD(TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, src->base[0] >> 8);
        if (src->format->planes > 1)
            TEGRA_VIC_METHOD(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, src->base[1] >> 8);
        if (src->format->planes > 2)
            TEGRA_VIC_METHOD(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_V, src->base[2] >> 8);
    }
    TEGRA_VIC_METHOD(TEGRA_VIC_M_EXECUTE, TEGRA_VIC_EXECUTE_AWAKEN);
#undef TEGRA_VIC_METHOD
    job->count = n;
    job->pixels = (uint64_t)op->dstW * op->dstH + (op->fill ? 0 : (op->srcW >> 16) * (op->srcH >> 16));
    return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
}

#ifdef __cplusplus
}
#endif
#endif
