// SPDX-License-Identifier: BSD-2-Clause-Patent
// The IOSurface pixel formats VIC 4.0 reads and writes, and the plane geometry and chroma siting each implies. Plain
// C, header only, included by TegraVICConfig.h.
//
// Format codes are NVB0B6_T_* (class 0xB0B6, T210's VIC 4.0): libdrm tests/tegra/vic.h and cyndis/vaapi-tegra-driver
// engine_headers/vic04.h (NVIDIA, MIT) for A8R8G8B8 32 and Y8___U8V8_N420 67; averne's FFmpeg nvtegra
// libavutil/clb0b6.h for the rest of the table (the values NVIDIA's open-gpu-doc vic_ceb6_types.h also gives VIC 5.1),
// and its hwcontext_nvtegra.c av_nvtegra_pixfmt_to_vic for the formats it runs on T210 (L8, NV12, YUV420P, R5G6B5 and
// the 32-bit RGB orders). N422 halves chroma across. That N422R ("rotated") halves it down is inferred from the name,
// weakly backed by L4T's T21x display bandwidth code (t21x/bandwidth.c), which gives a rotated 422R window the line
// count of an unrotated 422 one. A row counts elements: one per pixel in every packed format (averne: OutLumaWidth =
// linesize / step - 1), one CbCr pair in a semi-planar chroma plane.
#ifndef TEGRA_VIC_FORMATS_H
#define TEGRA_VIC_FORMATS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_VIC_T_L8 = 1,
    TEGRA_VIC_T_R8 = 4,
    TEGRA_VIC_T_R5G6B5 = 10,
    TEGRA_VIC_T_A8B8G8R8 = 31,
    TEGRA_VIC_T_A8R8G8B8 = 32,
    TEGRA_VIC_T_B8G8R8A8 = 33,
    TEGRA_VIC_T_R8G8B8A8 = 34,
    TEGRA_VIC_T_X8B8G8R8 = 35,
    TEGRA_VIC_T_X8R8G8B8 = 36,
    TEGRA_VIC_T_A2B10G10R10 = 39,
    TEGRA_VIC_T_A2R10G10B10 = 40,
    TEGRA_VIC_T_B10G10R10A2 = 41,
    TEGRA_VIC_T_R10G10B10A2 = 42,
    TEGRA_VIC_T_Y8_U8__Y8_V8 = 57,
    TEGRA_VIC_T_Y8_V8__Y8_U8 = 58,
    TEGRA_VIC_T_U8_Y8__V8_Y8 = 59,
    TEGRA_VIC_T_V8_Y8__U8_Y8 = 60,
    TEGRA_VIC_T_Y8___U8V8_N444 = 61,
    TEGRA_VIC_T_Y8___V8U8_N444 = 62,
    TEGRA_VIC_T_Y8___U8V8_N422 = 63,
    TEGRA_VIC_T_Y8___V8U8_N422 = 64,
    TEGRA_VIC_T_Y8___U8V8_N422R = 65,
    TEGRA_VIC_T_Y8___V8U8_N422R = 66,
    TEGRA_VIC_T_Y8___U8V8_N420 = 67,
    TEGRA_VIC_T_Y8___V8U8_N420 = 68,
    TEGRA_VIC_T_Y8___U8___V8_N444 = 69,
    TEGRA_VIC_T_Y8___U8___V8_N422 = 70,
    TEGRA_VIC_T_Y8___U8___V8_N422R = 71,
    TEGRA_VIC_T_Y8___U8___V8_N420 = 72,
};

#define TEGRA_VIC_MAX_PLANES 3

#define TEGRA_VIC_FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum {
    TEGRA_VIC_KIND_RGB = 0,
    TEGRA_VIC_KIND_YCC_VIDEO = 1,
    TEGRA_VIC_KIND_YCC_FULL = 2,
};

typedef struct {
    uint32_t fourcc;
    uint8_t vic, planes, kind, alpha;
    uint8_t bpe[TEGRA_VIC_MAX_PLANES];  // bytes per element of each plane (a CbCr pair is one element)
    uint8_t subX, subY;                 // chroma halved across (4:2:0, 4:2:2), down (4:2:0)
    // The code for the same memory with x and y exchanged: what an output VIC writes transposed is described as, since
    // the engine takes the whole output surface in the composed picture's coordinates (kTegraVICTraits). 0: no layout
    // describes it (packed 4:2:2 has no rotated form).
    uint8_t vicTransposed;
    uint8_t packed;  // 4:2:2 in one plane, two pixels sharing a chroma pair (TegraVICTraits.packedYcc)
} TegraVICFormat;

static inline const TegraVICFormat *tegra_vic_format(uint32_t fourcc)
{
    // 'BGRA' is bytes B, G, R, A: the 32-bit word 0xAARRGGBB, NVIDIA's A8R8G8B8. 'RGBA' is A8B8G8R8. 'l10r' is
    // CoreVideo's ARGB2101010LEPacked, A in the word's top two bits: A2R10G10B10. 'L565' is the 16-bit word with red on
    // top, R5G6B5; 'L008' one luminance byte. '420v'/'420f' are NV12, '422v'/'422f' NV16 and '444v'/'444f' NV24 (Cb
    // then Cr in the second plane); 'y420'/'f420' three planes, Y then Cb then Cr; '2vuy' is Cb Y0 Cr Y1, 'yuvs' Y0 Cb
    // Y1 Cr.
    static const TegraVICFormat formats[] = {
        {TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'),
         TEGRA_VIC_T_A8R8G8B8,
         1,
         TEGRA_VIC_KIND_RGB,
         1,
         {4, 0, 0},
         0,
         0,
         TEGRA_VIC_T_A8R8G8B8,
         0},
        {TEGRA_VIC_FOURCC('R', 'G', 'B', 'A'),
         TEGRA_VIC_T_A8B8G8R8,
         1,
         TEGRA_VIC_KIND_RGB,
         1,
         {4, 0, 0},
         0,
         0,
         TEGRA_VIC_T_A8B8G8R8,
         0},
        {TEGRA_VIC_FOURCC('l', '1', '0', 'r'),
         TEGRA_VIC_T_A2R10G10B10,
         1,
         TEGRA_VIC_KIND_RGB,
         1,
         {4, 0, 0},
         0,
         0,
         TEGRA_VIC_T_A2R10G10B10,
         0},
        {TEGRA_VIC_FOURCC('L', '5', '6', '5'),
         TEGRA_VIC_T_R5G6B5,
         1,
         TEGRA_VIC_KIND_RGB,
         0,
         {2, 0, 0},
         0,
         0,
         TEGRA_VIC_T_R5G6B5,
         0},
        {TEGRA_VIC_FOURCC('L', '0', '0', '8'),
         TEGRA_VIC_T_L8,
         1,
         TEGRA_VIC_KIND_RGB,
         0,
         {1, 0, 0},
         0,
         0,
         TEGRA_VIC_T_L8,
         0},
        {TEGRA_VIC_FOURCC('4', '2', '0', 'v'),
         TEGRA_VIC_T_Y8___U8V8_N420,
         2,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {1, 2, 0},
         1,
         1,
         TEGRA_VIC_T_Y8___U8V8_N420,
         0},
        {TEGRA_VIC_FOURCC('4', '2', '0', 'f'),
         TEGRA_VIC_T_Y8___U8V8_N420,
         2,
         TEGRA_VIC_KIND_YCC_FULL,
         0,
         {1, 2, 0},
         1,
         1,
         TEGRA_VIC_T_Y8___U8V8_N420,
         0},
        {TEGRA_VIC_FOURCC('4', '2', '2', 'v'),
         TEGRA_VIC_T_Y8___U8V8_N422,
         2,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {1, 2, 0},
         1,
         0,
         TEGRA_VIC_T_Y8___U8V8_N422R,
         0},
        {TEGRA_VIC_FOURCC('4', '2', '2', 'f'),
         TEGRA_VIC_T_Y8___U8V8_N422,
         2,
         TEGRA_VIC_KIND_YCC_FULL,
         0,
         {1, 2, 0},
         1,
         0,
         TEGRA_VIC_T_Y8___U8V8_N422R,
         0},
        {TEGRA_VIC_FOURCC('4', '4', '4', 'v'),
         TEGRA_VIC_T_Y8___U8V8_N444,
         2,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {1, 2, 0},
         0,
         0,
         TEGRA_VIC_T_Y8___U8V8_N444,
         0},
        {TEGRA_VIC_FOURCC('4', '4', '4', 'f'),
         TEGRA_VIC_T_Y8___U8V8_N444,
         2,
         TEGRA_VIC_KIND_YCC_FULL,
         0,
         {1, 2, 0},
         0,
         0,
         TEGRA_VIC_T_Y8___U8V8_N444,
         0},
        {TEGRA_VIC_FOURCC('y', '4', '2', '0'),
         TEGRA_VIC_T_Y8___U8___V8_N420,
         3,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {1, 1, 1},
         1,
         1,
         TEGRA_VIC_T_Y8___U8___V8_N420,
         0},
        {TEGRA_VIC_FOURCC('f', '4', '2', '0'),
         TEGRA_VIC_T_Y8___U8___V8_N420,
         3,
         TEGRA_VIC_KIND_YCC_FULL,
         0,
         {1, 1, 1},
         1,
         1,
         TEGRA_VIC_T_Y8___U8___V8_N420,
         0},
        {TEGRA_VIC_FOURCC('2', 'v', 'u', 'y'),
         TEGRA_VIC_T_U8_Y8__V8_Y8,
         1,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {2, 0, 0},
         1,
         0,
         0,
         1},
        {TEGRA_VIC_FOURCC('y', 'u', 'v', 's'),
         TEGRA_VIC_T_Y8_U8__Y8_V8,
         1,
         TEGRA_VIC_KIND_YCC_VIDEO,
         0,
         {2, 0, 0},
         1,
         0,
         0,
         1},
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {  // NOLINT(modernize-loop-convert): C too
        if (formats[i].fourcc == fourcc)
            return &formats[i];
    }
    return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
}

static inline uint32_t tegra_vic_plane_width(const TegraVICFormat *f, uint32_t width, uint32_t plane)
{
    return plane && f->subX ? width / 2 : width;
}

static inline uint32_t tegra_vic_plane_rows(const TegraVICFormat *f, uint32_t height, uint32_t plane)
{
    return plane && f->subY ? height / 2 : height;
}

// IOSurfaceAccelerator's siting codes 1 Left, 2 Center, 3 TopLeft, 4 Top, 5 BottomLeft, 6 Bottom (0 unspecified: Left)
// as VIC's ChromaLocHoriz/Vert (0 on the even sample, 1 between, 2 on the odd one), for an axis that is subsampled.
static inline uint32_t tegra_vic_chroma_horiz(uint32_t site)
{
    return (site == 2 || site == 4 || site == 6) ? 1 : 0;
}

static inline uint32_t tegra_vic_chroma_vert(uint32_t site)
{
    if (site == 3 || site == 4)
        return 0;
    if (site == 5 || site == 6)
        return 2;
    return 1;
}

// ChromaLocHoriz and ChromaLocVert of a surface of format `f` sited `site` (0 on an axis it does not subsample). A
// transposed 4:2:2 output's are its memory's exchanged, like the rest of its description; transposed 4:2:0 keeps the
// siting its jobs ran with on the unit.
static inline void tegra_vic_chroma_loc(const TegraVICFormat *f, uint32_t site, int transposed, uint32_t *horiz,
                                        uint32_t *vert)
{
    const uint32_t across = f->subX ? tegra_vic_chroma_horiz(site) : 0;
    const uint32_t down = f->subY ? tegra_vic_chroma_vert(site) : 0;
    const int exchange = transposed && f->subX != f->subY;
    *horiz = exchange ? down : across;
    *vert = exchange ? across : down;
}

#ifdef __cplusplus
}
#endif
#endif
