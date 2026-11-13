// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOSurfaceAccelerator's transform request as the kernel receives it (selector 1 of IOSurfaceAcceleratorClient, 0x1b0
// bytes, built by IOSurfaceAccelerator.framework's _prepareTransformBuffersAndOptions), and its translation into VIC
// jobs. Plain C, header only: TegraVICUserClient.cpp and the host test (test/vic_host.c) compile the same code.
// CONTRACT.md (drivers/TegraVIC) has the whole user-client contract and where each part of it was read.
#ifndef TEGRA_VIC_REQUEST_H
#define TEGRA_VIC_REQUEST_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "TegraVICConfig.h"
#include "SwitchSurfaceColor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in_type, pad0;
    uint64_t in_ptr;
    uint32_t in_size, out_type;
    uint64_t out_ptr;
    uint32_t out_size, pad1;
} TegraVICComm;

typedef struct {
    uint32_t src_id, dst_id;              // 0x000 IOSurfaceGetID of the source and the destination
    uint64_t cb_fn, cb_refcon, cb_info;   // 0x008 completion: callback, refcon, the one argument it is called with
    uint64_t flags;                       // 0x020 TEGRA_VIC_REQ_*
    uint64_t src_x, src_y, src_w, src_h;  // 0x028 source rectangle, 16.16 (w or h 0: the whole source)
    uint32_t est_src_w, est_src_h;        // 0x048 GetTransformEstimation only
    uint64_t rsvd50, rsvd58;              // 0x050 kernel clients' IOSurface pointers; zero from user space
    uint32_t dst_x, dst_y, dst_w, dst_h;  // 0x060 destination rectangle (w or h 0: the whole destination)
    uint32_t est_dst_w, est_dst_h;        // 0x070 GetTransformEstimation only
    uint32_t src_event_port, pad7c;       // 0x078 SharedEventSrcRef (a mach port name), 0 if none
    uint64_t src_wait, src_signal;        // 0x080
    uint32_t dst_event_port, pad94;       // 0x090 SharedEventDstRef
    uint64_t dst_wait, dst_signal;        // 0x098
    uint32_t alpha_override;              // 0x0a8
    uint32_t border_x, border_y, border_w, border_h;  // 0x0ac
    uint32_t border_alpha;                            // 0x0bc
    uint32_t border_r_y, border_g_cb, border_b_cr;    // 0x0c0
    uint16_t wo_alpha, wo_r_y, wo_g_cb, wo_b_cr;      // 0x0cc WriteOnlyMode values
    uint32_t rsvd_d4, rsvd_d8;                        // 0x0d4
    uint32_t hist_bin_mode;                           // 0x0dc
    uint32_t hist_x, hist_y, hist_w, hist_h;          // 0x0e0
    uint32_t hist_bins[8];                            // 0x0f0
    TegraVICComm comm[4];                             // 0x110 IOSurfaceAcceleratorCommApi records
} TegraVICRequest;

#ifdef __cplusplus
static_assert(sizeof(TegraVICRequest) == 0x1b0, "request");
static_assert(offsetof(TegraVICRequest, dst_x) == 0x60 && offsetof(TegraVICRequest, src_event_port) == 0x78 &&
                  offsetof(TegraVICRequest, alpha_override) == 0xa8 && offsetof(TegraVICRequest, wo_alpha) == 0xcc &&
                  offsetof(TegraVICRequest, hist_bin_mode) == 0xdc && offsetof(TegraVICRequest, comm) == 0x110,
              "request layout");
#else
_Static_assert(sizeof(TegraVICRequest) == 0x1b0, "request");
_Static_assert(offsetof(TegraVICRequest, dst_x) == 0x60 && offsetof(TegraVICRequest, src_event_port) == 0x78 &&
                   offsetof(TegraVICRequest, alpha_override) == 0xa8 && offsetof(TegraVICRequest, wo_alpha) == 0xcc &&
                   offsetof(TegraVICRequest, hist_bin_mode) == 0xdc && offsetof(TegraVICRequest, comm) == 0x110,
               "request layout");
#endif

// Flags (request +0x20; option keys of IOSurfaceAccelerator.framework).
#define TEGRA_VIC_REQ_TRANSFORM      0x7ull        // Transform: flip H, flip V, quarter turn
#define TEGRA_VIC_REQ_TRANSFORM_HIGH 0x38ull       // Transform values the framework lets through that name no flip
#define TEGRA_VIC_REQ_NEAREST        (1ull << 7)   // NearestFilter
#define TEGRA_VIC_REQ_SKIP_CALLBACK  (1ull << 10)  // SkipAsyncCallback
#define TEGRA_VIC_REQ_CHROMA_SITE(f) ((uint32_t)(((f) >> 15) & 7))
#define TEGRA_VIC_REQ_ALPHA_OVERRIDE (1ull << 18)
#define TEGRA_VIC_REQ_BORDER_FILL    (1ull << 28)
#define TEGRA_VIC_REQ_HISTOGRAM      (1ull << 29)
#define TEGRA_VIC_REQ_ZERO_FILL      (1ull << 30)
#define TEGRA_VIC_REQ_COLOR_REMAP    (7ull << 36)
#define TEGRA_VIC_REQ_HDR            (1ull << 40)
#define TEGRA_VIC_REQ_WRITE_ONLY     (1ull << 48)
#define TEGRA_VIC_REQ_FILM_GRAIN     (1ull << 49)

// What the kernel reads from an IOSurface (TegraVICSurface.cpp), or what the host test makes up.
typedef struct {
    uint32_t fourcc, width, height, planes;
    uint64_t planeOffset[TEGRA_VIC_MAX_PLANES], planeBytesPerRow[TEGRA_VIC_MAX_PLANES];
    uint64_t planeWidth[TEGRA_VIC_MAX_PLANES], planeHeight[TEGRA_VIC_MAX_PLANES];
    uint64_t length;      // the surface's memory descriptor
    uint64_t protection;  // IOSurface protection options
    uint32_t colorMatrix, transfer, primaries, chromaTop, chromaBottom;
} TegraVICSurfaceDesc;

enum {
    SWITCH_VIC_MATRIX_UNTAGGED,
    SWITCH_VIC_MATRIX_601,
    SWITCH_VIC_MATRIX_709,
    SWITCH_VIC_MATRIX_2020,
    SWITCH_VIC_MATRIX_UNSUPPORTED
};

static inline uint32_t switch_vic_matrix_tag(const char *tag)
{
    if (!tag)
        return SWITCH_VIC_MATRIX_UNTAGGED;
    if (!strcmp(tag, "ITU_R_601_4"))
        return SWITCH_VIC_MATRIX_601;
    if (!strcmp(tag, "ITU_R_709_2"))
        return SWITCH_VIC_MATRIX_709;
    if (!strcmp(tag, "ITU_R_2020"))
        return SWITCH_VIC_MATRIX_2020;
    return SWITCH_VIC_MATRIX_UNSUPPORTED;
}

// Bytes of the surface's memory the engine may touch: the end of its furthest plane.
static inline uint64_t tegra_vic_surface_extent(const TegraVICSurfaceDesc *d)
{
    uint64_t end = 0;
    const uint32_t planes = d->planes ? d->planes : 1;  // a surface without a plane array is one plane
    for (uint32_t p = 0; p < planes && p < TEGRA_VIC_MAX_PLANES; p++) {
        if (d->planeHeight[p] && d->planeBytesPerRow[p] > (UINT64_MAX - d->planeOffset[p]) / d->planeHeight[p])
            return UINT64_MAX;
        const uint64_t e = d->planeOffset[p] + d->planeBytesPerRow[p] * d->planeHeight[p];
        if (e > end)
            end = e;
    }
    return end;
}

// The surface as the engine sees it once mapped at `window` (its memory's first byte).
static inline const char *tegra_vic_image_from(const TegraVICSurfaceDesc *d, uint64_t window, TegraVICImage *im)
{
    memset(im, 0, sizeof(*im));
    im->format = tegra_vic_format(d->fourcc);
    if (!im->format)
        return "pixel format the scaler does not take";
    if (d->planes != im->format->planes && (d->planes != 0 || im->format->planes != 1))
        return "plane count does not match the pixel format";
    im->width = d->width;
    im->height = d->height;
    for (uint32_t p = 0; p < im->format->planes; p++) {
        if (d->planeBytesPerRow[p] > 0xffffffffu || d->planeHeight[p] > 0xffffffffu)
            return "plane geometry";
        if (d->planeOffset[p] > d->length || d->planeBytesPerRow[p] * d->planeHeight[p] > d->length - d->planeOffset[p])
            return "planes extend past the surface's memory";
        if (window > UINT64_MAX - d->planeOffset[p])
            return "plane address overflow";
        im->pitch[p] = (uint32_t)d->planeBytesPerRow[p];
        im->rows[p] = (uint32_t)d->planeHeight[p];
        im->base[p] = window + d->planeOffset[p];
        im->span[p] = d->length - d->planeOffset[p];
    }
    if (im->format->kind != TEGRA_VIC_KIND_RGB && d->colorMatrix >= SWITCH_VIC_MATRIX_UNSUPPORTED)
        return "unsupported YCbCr matrix attachment";
    // Untagged surfaces use BT.601; dimensions never override producer color metadata.
    im->standard = d->colorMatrix ? d->colorMatrix - 1 : TEGRA_VIC_YCC_601;
    im->transfer = d->transfer;
    im->primaries = d->primaries;
    if (im->format->kind != TEGRA_VIC_KIND_RGB &&
        !switch_surface_chroma_site(d->chromaTop, d->chromaBottom, &im->chromaSite))
        return "unsupported chroma location attachment";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
}

// Up to three jobs per request: fills first (ZeroFill, BorderFill), the transform (or WriteOnlyMode's fill) last.
typedef struct {
    TegraVICOp op[3];
    uint32_t count;
    uint32_t readsSource;  // some job reads the source surface
    uint32_t callback;     // the caller asked for an asynchronous completion
} TegraVICPlan;

enum {
    TEGRA_VIC_PLAN_OK = 0,
    TEGRA_VIC_PLAN_BAD = 1,
    TEGRA_VIC_PLAN_UNSUPPORTED = 2
};

static inline void tegra_vic_rgb_fill(TegraVICOp *op, uint32_t r8, uint32_t g8, uint32_t b8, uint32_t a8)
{
    op->fill = 1;
    op->fill10[0] = (r8 > 255 ? 255 : r8) << 2;
    op->fill10[1] = (g8 > 255 ? 255 : g8) << 2;
    op->fill10[2] = (b8 > 255 ? 255 : b8) << 2;
    op->fillAlpha10 = a8 >= 255 ? 1023 : a8 << 2;
}

// A fill of a subsampled surface covers whole chroma samples: its rectangle widened to even coordinates on each
// subsampled axis (Apple's driver rounds a border fill the same way), within the surface.
static inline void tegra_vic_even_rect(const TegraVICFormat *f, const TegraVICSurfaceDesc *d, TegraVICOp *o)
{
    if (!f->subX && !f->subY)
        return;
    const uint32_t mx = f->subX ? ~1u : ~0u;
    const uint32_t my = f->subY ? ~1u : ~0u;
    const uint32_t x0 = o->dstX & mx;
    const uint32_t y0 = o->dstY & my;
    uint32_t x1 = (o->dstX + o->dstW + (f->subX ? 1 : 0)) & mx;
    uint32_t y1 = (o->dstY + o->dstH + (f->subY ? 1 : 0)) & my;
    if (x1 > d->width)
        x1 = d->width & mx;
    if (y1 > d->height)
        y1 = d->height & my;
    o->dstX = x0;
    o->dstY = y0;
    o->dstW = x1 > x0 ? x1 - x0 : 0;
    o->dstH = y1 > y0 ? y1 - y0 : 0;
}

// Translates a request (surfaces already looked up) into jobs. Returns 0 and TEGRA_VIC_PLAN_OK, or why it is refused
// and whether that is a bad request or something this scaler does not do. Geometry is checked again, against the
// mapped surfaces, by tegra_vic_build.
static inline const char *tegra_vic_plan(const TegraVICRequest *r, const TegraVICSurfaceDesc *src,
                                         const TegraVICSurfaceDesc *dst, TegraVICPlan *plan, uint32_t *kind)
{
    memset(plan, 0, sizeof(*plan));
    *kind = TEGRA_VIC_PLAN_BAD;
    const uint64_t f = r->flags;
    if (f & TEGRA_VIC_REQ_TRANSFORM_HIGH)
        return "transform value outside the 8 flips and rotations";
    *kind = TEGRA_VIC_PLAN_UNSUPPORTED;
    if (f & TEGRA_VIC_REQ_HISTOGRAM)
        return "histogram (none on VIC)";
    if (f & (TEGRA_VIC_REQ_HDR | TEGRA_VIC_REQ_FILM_GRAIN))
        return "HDR or film grain (capabilities say no)";
    if (f & TEGRA_VIC_REQ_COLOR_REMAP)
        return "colour remap (capabilities say no)";
    const TegraVICFormat *df = tegra_vic_format(dst->fourcc);
    const bool writeOnly = (f & TEGRA_VIC_REQ_WRITE_ONLY) != 0;
    if (!df || (!writeOnly && !tegra_vic_format(src->fourcc)))
        return "pixel format the scaler does not take";
    if ((f & TEGRA_VIC_REQ_ALPHA_OVERRIDE) && df->alpha)
        return "alpha override into a surface with alpha";
    // Apple's driver answers kIOReturnUnsupported here too.
    if (!writeOnly && (src->protection & ~dst->protection))
        return "protected source into a less protected destination";
    *kind = TEGRA_VIC_PLAN_BAD;
    // Rectangles: an empty one is the whole surface (as AppleM2ScalerCSCDriver reads them).
    uint32_t dx = r->dst_x;
    uint32_t dy = r->dst_y;
    uint32_t dw = r->dst_w;
    uint32_t dh = r->dst_h;
    if (!dw || !dh) {
        dx = dy = 0;
        dw = dst->width;
        dh = dst->height;
    }
    if (f & TEGRA_VIC_REQ_ZERO_FILL) {
        TegraVICOp *o = &plan->op[plan->count++];
        o->dstW = dst->width;
        o->dstH = dst->height;
        tegra_vic_rgb_fill(o, 0, 0, 0, 255);
    }
    if (f & TEGRA_VIC_REQ_BORDER_FILL) {
        TegraVICOp *o = &plan->op[plan->count++];
        o->dstX = r->border_x;
        o->dstY = r->border_y;
        o->dstW = r->border_w;
        o->dstH = r->border_h;
        tegra_vic_rgb_fill(o, r->border_r_y, r->border_g_cb, r->border_b_cr, r->border_alpha);
        tegra_vic_even_rect(df, dst, o);
    }
    TegraVICOp *o = &plan->op[plan->count++];
    o->dstX = dx;
    o->dstY = dy;
    o->dstW = dw;
    o->dstH = dh;
    if (writeOnly) {
        // Core Animation's border clears send (255, 0, 0, 0) for every destination format: RGB, converted.
        tegra_vic_rgb_fill(o, r->wo_r_y, r->wo_g_cb, r->wo_b_cr, r->wo_alpha);
        tegra_vic_even_rect(df, dst, o);
    } else {
        o->srcX = r->src_x;
        o->srcY = r->src_y;
        o->srcW = r->src_w;
        o->srcH = r->src_h;
        if (!o->srcW || !o->srcH) {
            o->srcX = o->srcY = 0;
            o->srcW = (uint64_t)src->width << 16;
            o->srcH = (uint64_t)src->height << 16;
        }
        o->transform = (uint32_t)(f & TEGRA_VIC_REQ_TRANSFORM);
        o->nearest = (f & TEGRA_VIC_REQ_NEAREST) ? 1 : 0;
        plan->readsSource = 1;
    }
    plan->callback = (r->cb_fn != 0 && (f & TEGRA_VIC_REQ_SKIP_CALLBACK) == 0) ? 1 : 0;
    *kind = TEGRA_VIC_PLAN_OK;
    return NULL;  // NOLINT(modernize-use-nullptr): C too (vic_host.c)
}

#ifdef __cplusplus
}
#endif
#endif
