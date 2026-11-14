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
    // '422f' and '444f': the planes start in the service's own buffer of `repackBytes`, not the destination, and VIC
    // re-packs them into the destination at the request's offset
    uint32_t repack;
    uint64_t repackBytes;
} TegraNVJPGPlan;

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
    p->repack = 1;
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
    for (uint32_t i = 0; i < planes && i < 3; i++) {
        if (d->planeBytesPerRow[i] > 0xffffffffULL)
            return "stride";
        t->pitch[i] = (uint32_t)d->planeBytesPerRow[i];
    }
    nvjpg_dec_drv_pic_setup_s scratch;
    memset(&scratch, 0, sizeof(scratch));
    const char *why = tegra_nvjpg_setup_output(img, t, a->clipped == 0, &scratch, &p->extent);
    if (why)
        return why;
    // Where each engine plane starts: the destination's plane plus the picture's offset in it.
    uint32_t bpp = 4;
    if (format == TEGRA_NVJPG_OUT_YUY2) {
        bpp = 2;
    } else if (tegra_nvjpg_is_yuv(format) || format == TEGRA_NVJPG_OUT_GREY) {
        bpp = 1;
    }
    if (format == TEGRA_NVJPG_OUT_NV12 && ((r->x_offset | r->y_offset) & 1))
        return "odd offset into 4:2:0";
    if (format == TEGRA_NVJPG_OUT_YUY2 && (r->x_offset & 1))
        return "odd offset into 4:2:2";
    p->planes = p->extent.planes;
    p->planeStart[0] = d->planeOffset[0] + (uint64_t)r->y_offset * t->pitch[0] + (uint64_t)r->x_offset * bpp;
    if (format == TEGRA_NVJPG_OUT_NV12)
        p->planeStart[1] = d->planeOffset[1] + (uint64_t)(r->y_offset / 2) * t->pitch[1] + r->x_offset;
    for (uint32_t i = 0; i < p->planes; i++) {
        if (p->planeStart[i] & 0xff)
            return "a plane does not start on 256 bytes (NVJPG takes address >> 8)";
    }
    // Each plane's writes stay off every other plane, and inside the surface plus the sink.
    for (uint32_t i = 0; i < p->planes; i++) {
        const uint64_t end = p->planeStart[i] + (uint64_t)(p->extent.rows[i] - 1) * t->pitch[i] + p->extent.rowBytes[i];
        if (end > p->writeEnd)
            p->writeEnd = end;
        for (uint32_t j = 0; j < planes; j++) {
            if (j == i)
                continue;
            const uint64_t start = d->planeOffset[j];
            const uint64_t stop = start + d->planeBytesPerRow[j] * d->planeHeight[j];
            if (p->planeStart[i] < stop && end > start)
                return "writes would run into another plane";
        }
    }
    // The picture itself lies inside the surface; only the MCU padding past its last row may reach the sink.
    for (uint32_t i = 0; i < p->planes; i++) {
        const uint64_t rows = i && format == TEGRA_NVJPG_OUT_NV12 ? (r->out_height + 1) / 2 : r->out_height;
        const uint64_t pairs = 2 * (((uint64_t)r->out_width + 1) / 2);
        uint64_t bytes = (uint64_t)r->out_width * bpp;
        if (format == TEGRA_NVJPG_OUT_YUY2) {
            bytes = 2 * pairs;
        } else if (i && format == TEGRA_NVJPG_OUT_NV12) {
            bytes = pairs;
        }
        if (p->planeStart[i] + (rows - 1) * t->pitch[i] + bytes > d->length) {
            *kind = TEGRA_NVJPG_PLAN_BAD;
            return "picture runs past the destination";
        }
    }
    if (p->writeEnd > d->length + sink)
        return "MCU padding would run past the destination and its sink";
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

// Plan `p` (rows) moved onto a destination GM20B laid out block-linear (tegra_gm20b_surface_layout): `rowBytes` a row
// of GOBs, 2^`yLog2` GOBs a block, blocks row by row. The picture's offset must fall on a block: x on a GOB's 64 bytes,
// y on a block's rows. The engine may touch every block of the block rows its rows reach. NULL with `p` and the setup's
// output stride and tiling rewritten, or why the decode cannot write those blocks.
static inline const char *tegra_nvjpg_plan_block_linear(const TegraNVJPGDecodeRequest *r, uint64_t length,
                                                        uint64_t sink, uint32_t rowBytes, uint32_t yLog2,
                                                        TegraNVJPGPlan *p, nvjpg_dec_drv_pic_setup_s *s)
{
    if (p->planes != 1 || p->fillChroma || yLog2 > 5)
        return "block-linear destination of more than one plane, or blocks over 32 GOBs";
    uint32_t bpp = 4;
    if (p->target.format == TEGRA_NVJPG_OUT_YUY2) {
        bpp = 2;
    } else if (p->target.format == TEGRA_NVJPG_OUT_GREY) {
        bpp = 1;
    }
    const uint64_t blockRows = 8ull << yLog2;
    const uint64_t blockBytes = 512ull << yLog2;
    const uint64_t blockRowBytes = (uint64_t)rowBytes * blockRows;
    const uint64_t x = (uint64_t)r->x_offset * bpp;
    if (!rowBytes || (rowBytes & 63))
        return "block-linear row not a whole number of GOBs";
    if ((x & 63) || r->y_offset % blockRows)
        return "picture offset off the destination's blocks";
    if (x + p->extent.rowBytes[0] > rowBytes)
        return "picture wider than the destination's blocks";
    const uint64_t firstBlockRow = r->y_offset / blockRows;
    const uint64_t pictureEnd = (firstBlockRow + (r->out_height + blockRows - 1) / blockRows) * blockRowBytes;
    if (pictureEnd > length)
        return "picture runs past the destination's blocks";
    p->target.pitch[0] = rowBytes;
    p->target.blockLinear = 1;
    p->target.blockHeightLog2 = yLog2;
    p->planeStart[0] = firstBlockRow * blockRowBytes + (x / 64) * blockBytes;
    p->writeEnd = (firstBlockRow + (p->extent.rows[0] + blockRows - 1) / blockRows) * blockRowBytes;
    if (p->writeEnd > length + sink)
        return "MCU padding would run past the destination's blocks and its sink";
    s->output_stride_luma = (int)rowBytes;
    s->tile_mode = TEGRA_NVJPG_TILE_TEGRA_BLOCK_LINEAR;
    s->block_linear_height = (int)yLog2;
    return NULL;
}

#ifdef __cplusplus
}
#endif
#endif
