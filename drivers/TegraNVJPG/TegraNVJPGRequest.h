// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleJPEGDriverUserClient's decode requests as the kernel receives them (selector 1, 0x58 bytes, from CMPhoto's and
// JPEGH1.videodecoder's JPEGDeviceInterface_decodeJPEG; selector 5, 0x1000 bytes, CMPhoto's decodeJPEGExt; selector 7,
// 0xda0 bytes, every decode on 24B119), and their
// translation into one NVJPG decode. Plain C, header only: TegraNVJPGUserClient.cpp and the host test
// (test/request_host.c) compile the same code. CONTRACT.md has the whole contract and where each part was read.
#ifndef TEGRA_NVJPG_REQUEST_H
#define TEGRA_NVJPG_REQUEST_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "TegraNVJPGJpeg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t src_id;       // 0x00 IOSurface holding the JPEG file
    uint32_t src_size;     // 0x04 its IOSurfaceGetAllocSize (selector 1) or min(alloc, data length) (selector 5)
    uint32_t dst_id;       // 0x08 the destination IOSurface
    uint32_t dst_size;     // 0x0c its IOSurfaceGetAllocSize
    uint32_t rsvd10;       // 0x10
    uint32_t width;        // 0x14 the JPEG's width ("pixelX")
    uint32_t height;       // 0x18 and height ("pixelY")
    uint32_t quality;      // 0x1c encode only
    uint32_t flags;        // 0x20 TEGRA_NVJPG_REQ_* (Apple's kext reads the low byte)
    uint32_t x_offset;     // 0x24 where the picture goes in the destination (< 0x2000)
    uint32_t y_offset;     // 0x28
    uint32_t subsampling;  // 0x2c TEGRA_NVJPG_SUB_*
    uint64_t callback;     // 0x30 0: synchronous; else an async completion to the registered notification port
    uint64_t refcon;       // 0x38 the callback's first argument
    uint64_t context;      // 0x40 the completion's one argument
    uint32_t trace;        // 0x48 kdebug request ID
    uint32_t out_width;    // 0x4c the decoded size (DCT scaling: the JPEG's size / 2^n, rounded up)
    uint32_t out_height;   // 0x50
    uint32_t rsvd54;       // 0x54 (selector 5: MCUs per row)
} TegraNVJPGDecodeRequest;

// Selector 5's additions (CMPhoto JPEGDecompressSurfaceHelper 0x1aaeecef8).
typedef struct {
    TegraNVJPGDecodeRequest base;             // 0x00..0x57 (0x54 MCUs per row)
    uint32_t mcu_rows;                        // 0x58
    uint32_t crop;                            // 0x5c 1: decode only the MCU rectangle below
    uint32_t crop_x, crop_y, crop_w, crop_h;  // 0x60 in MCUs
    uint32_t hal70, hal74, hal78;             // 0x70 (out: 0x70 engine microseconds, 0x74 engine cycles)
    uint32_t header_bytes;                    // 0x7c length of the header blob for Apple's codec (<= 0xf7c)
    uint32_t entropy_offset;                  // 0x80 where the scan starts in the source
    uint8_t header[0x1000 - 0x84];            // 0x84 Apple's codec registers (ignored: NVJPG parses the file itself)
} TegraNVJPGDecodeRequestExt;

// Selector 7, AppleJPEGDriverIOStruct2024 (0xda0 bytes): every synchronous CMPhoto decode on 24B119
// (JPEGDecompressSurfaceHelper 0x1a6b80c9c -> decodeJPEG2024 0x1a6b80d90) and JPEGH1.videodecoder's (0x2c4988d48).
// 0x00..0x57 are selector 1's, the crop selector 5's; the strip fields are written only on CMPhoto's multi-core path,
// which needs AppleJPEGNumCores >= 2. Apple's kernel (startDecoder2024 0xfffffff008b06178) writes back +0x4c/+0x50
// (the region decoded) and +0xc94 (microseconds); no caller reads them.
typedef struct {
    TegraNVJPGDecodeRequest base;             // 0x00..0x57 (0x54 MCUs per row, cropped decodes only)
    uint32_t mcu_rows;                        // 0x58
    uint32_t crop;                            // 0x5c
    uint32_t crop_x, crop_y, crop_w, crop_h;  // 0x60 in MCUs
    uint32_t strip_index, strip_more;         // 0x70
    uint32_t strip_entropy_bytes;             // 0x78
    uint32_t header_bytes;                    // 0x7c strip header blob length
    uint32_t entropy_offset;                  // 0x80
    uint8_t header[0xc00];                    // 0x84
    uint32_t core;                            // 0xc84 0 auto
    uint32_t stats[0x24 / 4];                 // 0xc88 out: engine statistics, +0xc94 microseconds
    uint32_t rsvdcac;                         // 0xcac
    uint8_t rsvd[0xda0 - 0xcb0];
} TegraNVJPGDecodeRequest2024;

#ifdef __cplusplus
static_assert(sizeof(TegraNVJPGDecodeRequest2024) == 0xda0 && offsetof(TegraNVJPGDecodeRequest2024, crop) == 0x5c &&
                  offsetof(TegraNVJPGDecodeRequest2024, header_bytes) == 0x7c &&
                  offsetof(TegraNVJPGDecodeRequest2024, core) == 0xc84 &&
                  offsetof(TegraNVJPGDecodeRequest2024, rsvdcac) == 0xcac,
              "decode request (2024)");
static_assert(sizeof(TegraNVJPGDecodeRequest) == 0x58 && offsetof(TegraNVJPGDecodeRequest, callback) == 0x30 &&
                  offsetof(TegraNVJPGDecodeRequest, out_width) == 0x4c,
              "decode request");
static_assert(sizeof(TegraNVJPGDecodeRequestExt) == 0x1000 && offsetof(TegraNVJPGDecodeRequestExt, crop) == 0x5c &&
                  offsetof(TegraNVJPGDecodeRequestExt, header) == 0x84,
              "decode request (Ext)");
#else
_Static_assert(sizeof(TegraNVJPGDecodeRequest2024) == 0xda0 && offsetof(TegraNVJPGDecodeRequest2024, crop) == 0x5c &&
                   offsetof(TegraNVJPGDecodeRequest2024, header_bytes) == 0x7c &&
                   offsetof(TegraNVJPGDecodeRequest2024, core) == 0xc84 &&
                   offsetof(TegraNVJPGDecodeRequest2024, rsvdcac) == 0xcac,
               "decode request (2024)");
_Static_assert(sizeof(TegraNVJPGDecodeRequest) == 0x58 && offsetof(TegraNVJPGDecodeRequest, callback) == 0x30 &&
                   offsetof(TegraNVJPGDecodeRequest, out_width) == 0x4c,
               "decode request");
_Static_assert(sizeof(TegraNVJPGDecodeRequestExt) == 0x1000 && offsetof(TegraNVJPGDecodeRequestExt, crop) == 0x5c &&
                   offsetof(TegraNVJPGDecodeRequestExt, header) == 0x84,
               "decode request (Ext)");
#endif

#define TEGRA_NVJPG_REQ_HIGH_SPEED 0x1u  // Apple's fast IDCT: NVJPG has one IDCT
#define TEGRA_NVJPG_REQ_ALIGNED    0x2u  // 4:2:0 with MCU-aligned planes (Apple's no-zero-fill prepare)
#define TEGRA_NVJPG_REQ_NO_EOI     0x4u  // the bitstream has no FFD9 (AppleJPEGSupportsMissingEOI)

// The subsampling code userland sends (CMPhoto JPEGParseJPEGInfo, JPEGH1 checkAndMaybeUpdate...): Y 2x2, 2x1, 1x1,
// 4x1, grey. 4:4:0 has no code (CMPhoto never sends one).
enum {
    TEGRA_NVJPG_SUB_420 = 0,
    TEGRA_NVJPG_SUB_422 = 1,
    TEGRA_NVJPG_SUB_444 = 2,
    TEGRA_NVJPG_SUB_411 = 3,
    TEGRA_NVJPG_SUB_GREY = 4
};

#define TEGRA_NVJPG_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

// The destination surface as the kernel reads it (TegraVIC's IOSurface accessors), or as the host test makes it up.
typedef struct {
    uint32_t fourcc, width, height, planes;  // planes 0: a surface without a plane array (one implicit plane)
    uint64_t planeOffset[3], planeBytesPerRow[3], planeHeight[3];
    uint64_t length;      // what can be mapped: min(descriptor length, allocation size)
    uint64_t protection;  // IOSurface protection options (nonzero: protected content)
} TegraNVJPGSurface;

// What the engine decodes right (TegraNVJPGTraits, in a form plain C can take).
typedef struct {
    uint32_t formats, chroma, scales, clipped, greyRgb;
} TegraNVJPGAbility;

// A planned decode: the target, where each plane's first written byte is in the destination, how far writes may run
// (past the surface's own pages the output window maps a sink page), and the CPU's part.
typedef struct {
    TegraNVJPGTarget target;
    TegraNVJPGExtent extent;
    uint32_t planes;
    uint64_t planeStart[3];  // byte offsets in the destination surface (256-byte multiples)
    uint64_t writeEnd;       // one past the last byte the engine may write
    uint32_t fillChroma;     // grey into '420f': the CPU fills the CbCr plane with 0x80 after the decode
    uint64_t fillStart, fillRowBytes, fillPitch, fillRows;
    // '422f' and '444f': VIC re-packs NVJPG's 3-plane output (TEGRA_NVJPG_REPACK_VIC).
    // Pitch/block layout mismatches: Copy Engine DMA-repacks NVJPG's 64B-aligned output (TEGRA_NVJPG_REPACK_CE).
    uint32_t repack;
    uint64_t repackBytes;
    uint64_t copyDstStart[2];
    uint32_t copyDstPitch[2], copyRowBytes[2], copyRows[2];
} TegraNVJPGPlan;

enum {
    TEGRA_NVJPG_REPACK_NONE = 0,
    TEGRA_NVJPG_REPACK_VIC = 1,
    TEGRA_NVJPG_REPACK_CE = 2
};

enum {
    TEGRA_NVJPG_PLAN_OK = 0,
    TEGRA_NVJPG_PLAN_UNSUPPORTED = 1,
    TEGRA_NVJPG_PLAN_BAD = 2
};

static inline uint32_t tegra_nvjpg_sub_of(const TegraNVJPGImage *img)
{
    switch (img->chroma) {
    case TEGRA_NVJPG_CHROMA_420:
        return TEGRA_NVJPG_SUB_420;
    case TEGRA_NVJPG_CHROMA_422H:
        return TEGRA_NVJPG_SUB_422;
    case TEGRA_NVJPG_CHROMA_444:
        return TEGRA_NVJPG_SUB_444;
    case TEGRA_NVJPG_CHROMA_MONO:
        return TEGRA_NVJPG_SUB_GREY;
    default:
        return 0xffffffffu;  // 4:4:0: no code
    }
}

// The TEGRA_NVJPG_OUT_* a destination pixel format asks for and its plane count, or TEGRA_NVJPG_OUT_COUNT.
static inline uint32_t tegra_nvjpg_format_of(uint32_t fourcc, bool grey, uint32_t *planes)
{
    *planes = 1;
    switch (fourcc) {
    case TEGRA_NVJPG_FOURCC('4', '2', '0', 'f'):
        *planes = 2;
        return grey ? TEGRA_NVJPG_OUT_GREY : TEGRA_NVJPG_OUT_NV12;
    case TEGRA_NVJPG_FOURCC('B', 'G', 'R', 'A'):
        return TEGRA_NVJPG_OUT_BGRA;
    case TEGRA_NVJPG_FOURCC('R', 'G', 'B', 'A'):
        return TEGRA_NVJPG_OUT_RGBA;
    case 0x20:  // kCVPixelFormatType_32ARGB
        return TEGRA_NVJPG_OUT_ARGB;
    case TEGRA_NVJPG_FOURCC('4', '2', '2', 'f'):
        *planes = 2;
        return TEGRA_NVJPG_OUT_NV16;
    case TEGRA_NVJPG_FOURCC('4', '4', '4', 'f'):
        *planes = 2;
        return TEGRA_NVJPG_OUT_NV24;
    case TEGRA_NVJPG_FOURCC('y', 'u', 'v', 'f'):
        return TEGRA_NVJPG_OUT_YUY2;
    default:
        return TEGRA_NVJPG_OUT_COUNT;
    }
}

static inline uint64_t tegra_nvjpg_align(uint64_t value, uint64_t to)
{
    return (value + to - 1) / to * to;
}

static inline uint32_t tegra_nvjpg_bpp_of(uint32_t format)
{
    if (format == TEGRA_NVJPG_OUT_YUY2)
        return 2;
    if (tegra_nvjpg_is_yuv(format) || format == TEGRA_NVJPG_OUT_GREY)
        return 1;
    return 4;
}

static inline void tegra_nvjpg_picture_plane(const TegraNVJPGDecodeRequest *r, uint32_t format, uint32_t bpp,
                                             uint32_t plane, uint64_t *rows, uint64_t *bytes)
{
    *rows = plane && format == TEGRA_NVJPG_OUT_NV12 ? (r->out_height + 1) / 2 : r->out_height;
    const uint64_t pairs = 2 * (((uint64_t)r->out_width + 1) / 2);
    *bytes = (uint64_t)r->out_width * bpp;
    if (format == TEGRA_NVJPG_OUT_YUY2) {
        *bytes = 2 * pairs;
    } else if (plane && format == TEGRA_NVJPG_OUT_NV12) {
        *bytes = pairs;
    }
}

static inline uint64_t tegra_nvjpg_dst_plane_start(const TegraNVJPGDecodeRequest *r, const TegraNVJPGSurface *d,
                                                   uint32_t format, uint32_t bpp, uint32_t plane)
{
    if (plane && format == TEGRA_NVJPG_OUT_NV12)
        return d->planeOffset[1] + (uint64_t)(r->y_offset / 2) * d->planeBytesPerRow[1] + r->x_offset;
    return d->planeOffset[0] + (uint64_t)r->y_offset * d->planeBytesPerRow[0] + (uint64_t)r->x_offset * bpp;
}

// Stage pitch-linear NVJPG output in a 64-byte-aligned scratch buffer, then DMA-repack to `d` with Copy Engine.
// Returns NULL on success with `p` filled for TEGRA_NVJPG_REPACK_CE; otherwise returns why CE cannot take it.
static inline const char *tegra_nvjpg_plan_stage_ce(const TegraNVJPGDecodeRequest *r, const TegraNVJPGImage *img,
                                                    const TegraNVJPGSurface *d, const TegraNVJPGAbility *a,
                                                    uint32_t surfacePlanes, uint32_t bpp, TegraNVJPGPlan *p)
{
    TegraNVJPGPlan staged;
    memset(&staged, 0, sizeof(staged));
    staged.target.format = p->target.format;
    staged.target.scale = p->target.scale;
    staged.target.alpha = p->target.alpha;
    const uint32_t format = staged.target.format;
    const uint32_t w = a->clipped ? tegra_nvjpg_shr_up(img->width, staged.target.scale)
                                  : (img->mcusX * img->mcuW) >> staged.target.scale;
    const uint32_t cw = (w + 1) / 2;
    if (format == TEGRA_NVJPG_OUT_NV12) {
        staged.target.pitch[0] = (uint32_t)tegra_nvjpg_align(w, 64);
        staged.target.pitch[1] = (uint32_t)tegra_nvjpg_align(2ull * cw, 64);
    } else if (format == TEGRA_NVJPG_OUT_YUY2) {
        staged.target.pitch[0] = (uint32_t)tegra_nvjpg_align(4ull * cw, 64);
    } else if (format == TEGRA_NVJPG_OUT_GREY) {
        staged.target.pitch[0] = (uint32_t)tegra_nvjpg_align(w, 64);
    } else {
        staged.target.pitch[0] = (uint32_t)tegra_nvjpg_align(4ull * w, 64);
    }
    nvjpg_dec_drv_pic_setup_s scratch;
    memset(&scratch, 0, sizeof(scratch));
    const char *why = tegra_nvjpg_setup_output(img, &staged.target, a->clipped == 0, &scratch, &staged.extent);
    if (why)
        return why;
    staged.planes = staged.extent.planes;
    if (staged.planes > 2)
        return "too many planes for copy-engine repack";
    uint64_t end = 0;
    for (uint32_t i = 0; i < staged.planes; i++) {
        uint64_t rows = 0, bytes = 0;
        tegra_nvjpg_picture_plane(r, format, bpp, i, &rows, &bytes);
        const uint64_t dstStart = tegra_nvjpg_dst_plane_start(r, d, format, bpp, i);
        if ((dstStart & 15) || d->planeBytesPerRow[i] < bytes || rows > 0xffffffffULL || bytes > 0xffffffffULL)
            return "destination plane offset not 16-byte aligned";
        const uint64_t dstEnd = dstStart + (rows - 1) * d->planeBytesPerRow[i] + bytes;
        if (dstEnd > d->length)
            return "picture runs past the destination";
        for (uint32_t j = 0; j < surfacePlanes; j++) {
            if (j == i)
                continue;
            const uint64_t start = d->planeOffset[j];
            const uint64_t stop = start + d->planeBytesPerRow[j] * d->planeHeight[j];
            if (dstStart < stop && dstEnd > start)
                return "writes would run into another plane";
        }
        staged.planeStart[i] = end;
        end = tegra_nvjpg_align(end + (uint64_t)staged.target.pitch[i] * staged.extent.rows[i], 256);
        staged.copyDstStart[i] = dstStart;
        staged.copyDstPitch[i] = (uint32_t)d->planeBytesPerRow[i];
        staged.copyRowBytes[i] = (uint32_t)bytes;
        staged.copyRows[i] = (uint32_t)rows;
    }
    if (format == TEGRA_NVJPG_OUT_GREY) {
        staged.fillChroma = 1;
        staged.fillPitch = d->planeBytesPerRow[1];
        staged.fillStart = d->planeOffset[1] + (uint64_t)(r->y_offset / 2) * staged.fillPitch + r->x_offset;
        staged.fillRowBytes = 2 * (((uint64_t)r->out_width + 1) / 2);
        staged.fillRows = ((uint64_t)r->out_height + 1) / 2;
        if (staged.fillStart + (staged.fillRows - 1) * staged.fillPitch + staged.fillRowBytes > d->length)
            return "chroma fill would run past the destination";
    }
    staged.writeEnd = end;
    staged.repack = TEGRA_NVJPG_REPACK_CE;
    staged.repackBytes = tegra_nvjpg_align(end, 4096);
    *p = staged;
    return NULL;
}

// A direct single-plane plan moved onto NVJPG's rows in the service's scratch, which the copy engine then copies into
// the destination (`tiled`: whole, into the blocks GM20B laid it out in, tiledW x tiledH texels of tiledBpp bytes).
// True with `p` and `s` updated.
static inline bool tegra_nvjpg_plan_fallback_ce(const TegraNVJPGDecodeRequest *r, const TegraNVJPGSurface *d,
                                                uint64_t maxRepackBytes, bool tiled, uint32_t tiledW, uint32_t tiledH,
                                                uint32_t tiledBpp, bool compressed, TegraNVJPGPlan *p,
                                                nvjpg_dec_drv_pic_setup_s *s)
{
    if (p->repack != TEGRA_NVJPG_REPACK_NONE || p->planes != 1 || p->fillChroma || compressed)
        return false;
    const uint32_t format = p->target.format;
    const uint32_t bpp = tegra_nvjpg_bpp_of(format);
    uint64_t rows = 0, bytes = 0;
    tegra_nvjpg_picture_plane(r, format, bpp, 0, &rows, &bytes);
    const uint64_t dstStart = tegra_nvjpg_dst_plane_start(r, d, format, bpp, 0);
    if ((dstStart & 15) || d->planeBytesPerRow[0] > 0xffffffffULL || d->planeBytesPerRow[0] < bytes ||
        rows > 0xffffffffULL || bytes > 0xffffffffULL)
        return false;
    if (dstStart + (rows - 1) * d->planeBytesPerRow[0] + bytes > d->length)
        return false;
    if (tiled && (dstStart != 0 || (uint64_t)tiledW * tiledBpp != bytes || tiledH != rows))
        return false;
    const uint32_t scratchPitch = (uint32_t)tegra_nvjpg_align(p->extent.rowBytes[0], 64);
    const uint64_t end = tegra_nvjpg_align((uint64_t)scratchPitch * p->extent.rows[0], 256);
    const uint64_t repackBytes = tegra_nvjpg_align(end, 4096);
    if (!scratchPitch || !repackBytes || repackBytes > maxRepackBytes)
        return false;
    p->target.pitch[0] = scratchPitch;
    p->planeStart[0] = 0;
    p->writeEnd = end;
    p->repack = TEGRA_NVJPG_REPACK_CE;
    p->repackBytes = repackBytes;
    p->copyDstStart[0] = dstStart;
    p->copyDstPitch[0] = (uint32_t)d->planeBytesPerRow[0];
    p->copyRowBytes[0] = (uint32_t)bytes;
    p->copyRows[0] = (uint32_t)rows;
    s->output_stride_luma = (int)scratchPitch;
    return true;
}

// '422f' or '444f' (p->target's format, scale set): Y, Cb and Cr at the file's own sampling into the service's buffer,
// each plane's rows 256 bytes apart (VIC's source pitch) and each plane on 256 bytes. The destination is VIC's: its
// rectangle must start on a chroma pair. *kind is the caller's UNSUPPORTED until the plan is OK.
static inline const char *tegra_nvjpg_plan_repack(const TegraNVJPGDecodeRequest *r, const TegraNVJPGImage *img,
                                                  const TegraNVJPGAbility *a, TegraNVJPGPlan *p)
{
    TegraNVJPGTarget *t = &p->target;
    if (t->format == TEGRA_NVJPG_OUT_NV16 && (r->x_offset & 1))
        return "odd offset into 4:2:2";
    const uint32_t w = a->clipped ? tegra_nvjpg_shr_up(img->width, t->scale) : (img->mcusX * img->mcuW) >> t->scale;
    const uint32_t chroma = t->format == TEGRA_NVJPG_OUT_NV16 ? (w + 1) / 2 : w;
    t->pitch[0] = (uint32_t)tegra_nvjpg_align(w, 256);
    t->pitch[1] = t->pitch[2] = (uint32_t)tegra_nvjpg_align(chroma, 256);
    nvjpg_dec_drv_pic_setup_s scratch;
    memset(&scratch, 0, sizeof(scratch));
    const char *why = tegra_nvjpg_setup_output(img, t, a->clipped == 0, &scratch, &p->extent);
    if (why)
        return why;
    p->planes = p->extent.planes;
    uint64_t end = 0;
    for (uint32_t i = 0; i < p->planes; i++) {
        p->planeStart[i] = end;
        end = tegra_nvjpg_align(end + (uint64_t)t->pitch[i] * p->extent.rows[i], 256);
    }
    p->writeEnd = end;
    p->repack = TEGRA_NVJPG_REPACK_VIC;
    p->repackBytes = tegra_nvjpg_align(end, 4096);
    return NULL;
}

// The plan for request `r` (its JPEG parsed into `img`) into destination `d`, with what the engine proved. NULL and
// *kind OK, or the reason and *kind UNSUPPORTED (a file or shape NVJPG, or this unit, does not do: the caller decodes
// on the CPU) or BAD (the request contradicts itself or the surfaces). `sink`: bytes the output window can map past the
// destination's own pages.
static inline const char *tegra_nvjpg_plan(const TegraNVJPGDecodeRequest *r, const TegraNVJPGImage *img,
                                           const TegraNVJPGSurface *d, const TegraNVJPGAbility *a, uint64_t sink,
                                           TegraNVJPGPlan *p, uint32_t *kind)
{
    memset(p, 0, sizeof(*p));
    *kind = TEGRA_NVJPG_PLAN_BAD;
    // Apple's own bounds (startDecoder 0x45efc): destination size, the JPEG's and the decode's sizes, offsets.
    if (r->dst_size < 7 || r->width < 4 || r->height < 2 || r->out_width < 4 || r->out_height < 2 ||
        r->x_offset >= 0x2000 || r->y_offset >= 0x2000)
        return "sizes or offsets outside Apple's bounds";
    if (r->width != img->width || r->height != img->height)
        return "request size is not the file's";
    *kind = TEGRA_NVJPG_PLAN_UNSUPPORTED;
    if (r->subsampling != tegra_nvjpg_sub_of(img))
        return "subsampling code is not the file's (or 4:4:0)";
    if (!(a->chroma >> img->chroma & 1))
        return "sampling not proven on this unit";
    uint32_t scale = 0;
    while (scale < 4 && (tegra_nvjpg_shr_up(img->width, scale) != r->out_width ||
                         tegra_nvjpg_shr_up(img->height, scale) != r->out_height))
        scale++;
    if (scale == 4)
        return "decode size is not the file's size / 1, 2, 4 or 8";
    if (!(a->scales >> scale & 1))
        return "scale not proven on this unit";
    const bool grey = img->components == 1;
    uint32_t planes = 0;
    const uint32_t format = tegra_nvjpg_format_of(d->fourcc, grey, &planes);
    if (format == TEGRA_NVJPG_OUT_COUNT)
        return "destination pixel format (NVJPG writes 420f, yuvf, BGRA, RGBA, ARGB)";
    if (!(a->formats >> format & 1))
        return "output format NVJPG does not write on this unit";
    if (grey && format != TEGRA_NVJPG_OUT_GREY && !a->greyRgb)
        return "grey to RGB not proven on this unit";
    if (grey && format != TEGRA_NVJPG_OUT_GREY && tegra_nvjpg_is_yuv(format))
        return "grey file into a 4:2:2 or 4:4:4 destination";
    *kind = TEGRA_NVJPG_PLAN_BAD;
    if ((d->planes ? d->planes : 1) != planes)
        return "destination plane count";
    if (d->protection)
        return "protected destination";
    if ((uint64_t)r->x_offset + r->out_width > d->width || (uint64_t)r->y_offset + r->out_height > d->height)
        return "picture does not fit the destination";
    *kind = TEGRA_NVJPG_PLAN_UNSUPPORTED;
    TegraNVJPGTarget *t = &p->target;
    t->format = format;
    t->scale = scale;
    t->alpha = 0xff;
    if (format == TEGRA_NVJPG_OUT_NV16 || format == TEGRA_NVJPG_OUT_NV24) {
        const char *why = tegra_nvjpg_plan_repack(r, img, a, p);
        if (!why)
            *kind = TEGRA_NVJPG_PLAN_OK;
        return why;
    }
    if (format == TEGRA_NVJPG_OUT_NV12 && ((r->x_offset | r->y_offset) & 1))
        return "odd offset into 4:2:0";
    if (format == TEGRA_NVJPG_OUT_YUY2 && (r->x_offset & 1))
        return "odd offset into 4:2:2";
    const uint32_t bpp = tegra_nvjpg_bpp_of(format);
    const uint32_t enginePlanes = tegra_nvjpg_planes(format);
    for (uint32_t i = 0; i < planes && i < 3; i++) {
        if (d->planeBytesPerRow[i] > 0xffffffffULL)
            return "stride";
        t->pitch[i] = (uint32_t)d->planeBytesPerRow[i];
    }
    // The picture itself must fit in the destination regardless of whether NVJPG writes directly or stages via CE.
    for (uint32_t i = 0; i < enginePlanes; i++) {
        uint64_t rows = 0, bytes = 0;
        tegra_nvjpg_picture_plane(r, format, bpp, i, &rows, &bytes);
        if (d->planeBytesPerRow[i] < bytes) {
            *kind = TEGRA_NVJPG_PLAN_BAD;
            return "stride below the row";
        }
        const uint64_t start = tegra_nvjpg_dst_plane_start(r, d, format, bpp, i);
        if (start + (rows - 1) * d->planeBytesPerRow[i] + bytes > d->length) {
            *kind = TEGRA_NVJPG_PLAN_BAD;
            return "picture runs past the destination";
        }
    }
    nvjpg_dec_drv_pic_setup_s scratch;
    memset(&scratch, 0, sizeof(scratch));
    const char *directWhy = tegra_nvjpg_setup_output(img, t, a->clipped == 0, &scratch, &p->extent);
    if (!directWhy) {
        p->planes = p->extent.planes;
        for (uint32_t i = 0; i < p->planes; i++)
            p->planeStart[i] = tegra_nvjpg_dst_plane_start(r, d, format, bpp, i);
        for (uint32_t i = 0; i < p->planes; i++) {
            if (p->planeStart[i] & 0xff) {
                directWhy = "a plane does not start on 256 bytes (NVJPG takes address >> 8)";
                break;
            }
        }
    }
    if (!directWhy) {
        for (uint32_t i = 0; i < p->planes; i++) {
            const uint64_t end =
                p->planeStart[i] + (uint64_t)(p->extent.rows[i] - 1) * t->pitch[i] + p->extent.rowBytes[i];
            if (end > p->writeEnd)
                p->writeEnd = end;
            for (uint32_t j = 0; j < planes; j++) {
                if (j == i)
                    continue;
                const uint64_t start = d->planeOffset[j];
                const uint64_t stop = start + d->planeBytesPerRow[j] * d->planeHeight[j];
                if (p->planeStart[i] < stop && end > start) {
                    directWhy = "writes would run into another plane";
                    break;
                }
            }
            if (directWhy)
                break;
        }
    }
    if (!directWhy && p->writeEnd > d->length + sink)
        directWhy = "MCU padding would run past the destination and its sink";
    if (directWhy) {
        if (!tegra_nvjpg_plan_stage_ce(r, img, d, a, planes, bpp, p)) {
            *kind = TEGRA_NVJPG_PLAN_OK;
            return NULL;
        }
        return directWhy;
    }
    if (format == TEGRA_NVJPG_OUT_GREY) {
        // '420f' from a grey file: the chroma plane is the CPU's, neutral (0x80), over the picture's rows.
        p->fillChroma = 1;
        p->fillPitch = d->planeBytesPerRow[1];
        p->fillStart = d->planeOffset[1] + (uint64_t)(r->y_offset / 2) * p->fillPitch + r->x_offset;
        p->fillRowBytes = 2 * (((uint64_t)r->out_width + 1) / 2);
        p->fillRows = ((uint64_t)r->out_height + 1) / 2;
        if (p->fillStart + (p->fillRows - 1) * p->fillPitch + p->fillRowBytes > d->length)
            return "chroma fill would run past the destination";
    }
    *kind = TEGRA_NVJPG_PLAN_OK;
    return NULL;
}

#ifdef __cplusplus
}
#endif
#endif
