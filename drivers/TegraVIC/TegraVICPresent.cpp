// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's pictures into IOSurfaces on VIC (TegraVICPresent.h). The destination VIC sees is the picture's own rectangle
// of the surface: its first byte in each plane is the plane base, the surface's pitch is kept, so VIC writes nothing
// outside the rectangle and only the rows it covers are mapped into the destination window and made coherent.
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <string.h>
#include "TegraVIC.h"
#include "TegraVICBand.h"
#include "TegraVICPresent.h"
#include "TegraVICSurface.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr uint32_t kPresentPages = 8192;  // 32 MiB a side: a 4096x4096 NV12 picture, or the rows it covers
uint64_t *gPresentPages;                  // the destination rows' pages; the picture brings its own list
TegraVICJob gPresentJob;                  // under the engine lock

#if TEGRA_VIC_STATS
struct {
    uint64_t windowStart, pictures, refused, failed, engineUs, cpuUs, bytes;
} gPresentStats;  // under the engine lock, but the refusals (atomic)

// One line per 5 s window with pictures (engine lock held).
void presentStatsLine()
{
    const uint64_t now = switch_now_us();
    if (!gPresentStats.windowStart)
        gPresentStats.windowStart = now;
    if (now - gPresentStats.windowStart < 5000000)
        return;
    const uint64_t n = gPresentStats.pictures ? gPresentStats.pictures : 1;
    char line[200];
    snprintf(line, sizeof(line),
             "TegraVIC: %llus present=%llu refused=%llu failed=%llu engine=%lluus/picture cpu=%lluus/picture "
             "mapped=%lluKB/picture",
             (now - gPresentStats.windowStart) / 1000000, gPresentStats.pictures,
             __atomic_load_n(&gPresentStats.refused, __ATOMIC_RELAXED), gPresentStats.failed,
             gPresentStats.engineUs / n, gPresentStats.cpuUs / n, (gPresentStats.bytes >> 10) / n);
    os_log(OS_LOG_DEFAULT, "%s", line);
    bzero(&gPresentStats, sizeof(gPresentStats));
    gPresentStats.windowStart = now;
}
#endif

// `why` is a literal: the refusal's key as well as its text.
void presentRefused(const char *why)
{
#if TEGRA_VIC_STATS
    __atomic_add_fetch(&gPresentStats.refused, 1, __ATOMIC_RELAXED);
#endif
    switch_refusal("TegraVIC", why, "decoded picture not placed by VIC, the decoder copies it on the CPU");
}

// Where the picture lands: its size clipped to the destination, and the rows it covers in each plane.
struct Placement {
    TegraVICImage whole;  // the surface at address 0, for its format, pitches and colour standard
    uint32_t width, height;
    TegraVICBand band[2];
    uint32_t pages[2];
};

const char *place(const TegraVICPicture *p, const TegraVICSurfaceDesc *d, Placement *pl)
{
    const char *why = tegra_vic_image_from(d, 0, &pl->whole);
    if (why)
        return why;
    if (pl->whole.format->kind == TEGRA_VIC_KIND_RGB || pl->whole.format->planes != 2)
        return "destination is not 4:2:0";
    // Odd sizes are refused rather than rounded: the column or row VIC could not write would show what the buffer held.
    if ((p->cropX | p->cropY | p->dstX | p->dstY) & 1)
        return "picture window or placement on odd coordinates";
    if (p->dstX >= d->width || p->dstY >= d->height)
        return "picture placed outside the destination";
    pl->width = p->width < d->width - p->dstX ? p->width : d->width - p->dstX;
    pl->height = p->height < d->height - p->dstY ? p->height : d->height - p->dstY;
    if ((pl->width | pl->height) & 1)
        return "picture of odd size";
    if (pl->width < TEGRA_VIC_MIN_DIM || pl->height < TEGRA_VIC_MIN_DIM)
        return "picture under 2x2";
    if (!p->pages || !p->bytes || p->bytes > static_cast<uint64_t>(p->pageCount) << 12 ||
        p->bytes > static_cast<uint64_t>(kPresentPages) << 12)
        return "picture memory outside its page list or over 32 MiB";
    for (uint32_t k = 0; k < 2; k++) {
        pl->band[k] =
            tegra_vic_band_420(d->planeOffset[k], pl->whole.pitch[k], k, p->dstX, p->dstY, pl->width, pl->height);
        pl->pages[k] = tegra_vic_band_pages(&pl->band[k]);
    }
    if (static_cast<uint64_t>(pl->pages[0]) + pl->pages[1] > kPresentPages)
        return "destination rows over 32 MiB";
    return nullptr;
}

// The picture at `srcBase`, the destination rectangle's planes mapped from `dstBase` (luma pages, then chroma pages),
// and the 1:1 copy between them. The source takes the surface's format and colour standard, so the job is the 4:2:0
// pass-through (no matrix).
void describe(const TegraVICPicture *p, const Placement &pl, uint64_t srcBase, uint64_t dstBase, TegraVICImage *src,
              TegraVICImage *dst, TegraVICOp *op)
{
    bzero(dst, sizeof(*dst));
    dst->format = pl.whole.format;
    dst->width = pl.width;
    dst->height = pl.height;
    dst->standard = pl.whole.standard;
    dst->lastRowExact = 1;
    uint64_t window = dstBase;
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t inPage = static_cast<uint32_t>(pl.band[k].start & SWITCH_HW_PAGE_MASK);
        dst->pitch[k] = pl.band[k].pitch;
        dst->rows[k] = pl.band[k].rows;
        dst->base[k] = window + inPage;
        dst->span[k] = (static_cast<uint64_t>(pl.pages[k]) << SWITCH_HW_PAGE_SHIFT) - inPage;
        window += static_cast<uint64_t>(pl.pages[k]) << SWITCH_HW_PAGE_SHIFT;
    }
    bzero(src, sizeof(*src));
    src->format = pl.whole.format;
    src->width = p->cropX + pl.width;
    src->height = p->cropY + pl.height;
    src->pitch[0] = src->pitch[1] = p->pitch;
    src->rows[0] = p->rows;
    src->rows[1] = p->rows / 2;
    src->base[0] = srcBase;
    src->span[0] = p->chromaOffset;
    src->base[1] = srcBase + p->chromaOffset;
    src->span[1] = p->chromaOffset < p->bytes ? p->bytes - p->chromaOffset : 0;
    src->standard = pl.whole.standard;
    src->blockLinear = 1;
    src->blockHeightLog2 = p->blockHeightLog2;
    bzero(op, sizeof(*op));
    op->srcX = static_cast<uint64_t>(p->cropX) << 16;
    op->srcY = static_cast<uint64_t>(p->cropY) << 16;
    op->srcW = static_cast<uint64_t>(pl.width) << 16;
    op->srcH = static_cast<uint64_t>(pl.height) << 16;
    op->dstW = pl.width;
    op->dstH = pl.height;
    op->nearest = 1;  // 1:1
}

struct CacheRuns {
    switch_dcache_op op;
    const uint64_t *pages;
};

// NOLINTNEXTLINE(misc-const-correctness): the page walker's callback signature
void cacheRun(void *context, uint32_t page, uint32_t offset, uint32_t length)
{
    const CacheRuns *c = static_cast<const CacheRuns *>(context);
    const uint64_t pa = c->pages[page] + offset;
    if (switch_dma_pa_managed(pa))
        switch_dcache_phys_lines(c->op, pa, length);
}

// The rectangle's lines in both planes, then one barrier. A line it shares with pixels beside the rectangle is only
// ever clean in the CPU's caches here: every present cleans its own lines before VIC writes them.
void cacheRectangle(switch_dcache_op op, const Placement &pl)
{
    CacheRuns luma = {op, gPresentPages};
    CacheRuns chroma = {op, gPresentPages + pl.pages[0]};
    tegra_vic_band_runs(&pl.band[0], cacheRun, &luma);
    tegra_vic_band_runs(&pl.band[1], cacheRun, &chroma);
    switch_dcache_sync();
}

// The destination rows' pages: luma's band, then chroma's.
IOReturn listDestination(TegraVICSurface *surface, const Placement &pl)
{
    uint32_t listed = 0;
    uint32_t more = 0;
    IOReturn ret =
        tegra_vic_surface_pages(surface, pl.band[0].start, tegra_vic_band_end(&pl.band[0]) - pl.band[0].start,
                                gPresentPages, pl.pages[0], &listed);
    if (!ret) {
        ret = tegra_vic_surface_pages(surface, pl.band[1].start, tegra_vic_band_end(&pl.band[1]) - pl.band[1].start,
                                      gPresentPages + listed, pl.pages[1], &more);
    }
    return ret;
}
}  // namespace

extern "C" bool tegra_vic_present_ready(void)
{
    const TegraVICTraits *t = tegra_vic_traits();
    return (__atomic_load_n(&gPresentPages, __ATOMIC_ACQUIRE) != nullptr) && tegra_vic_ready() && (t != nullptr) &&
           (t->blockLinearIn != 0u);
}

// Only with block-linear input proven; the page list is the ready bit: without it every present is refused.
extern "C" void tegra_vic_present_start(void)
{
    if (!tegra_vic_traits()->blockLinearIn || __atomic_load_n(&gPresentPages, __ATOMIC_ACQUIRE))
        return;
    if (uint64_t *dst = IONew(uint64_t, kPresentPages))  // NOLINT(misc-const-correctness): gPresentPages takes it
        __atomic_store_n(&gPresentPages, dst, __ATOMIC_RELEASE);
}

extern "C" int tegra_vic_present(const TegraVICPicture *p, task_t task, const char **why)
{
    *why = nullptr;
    if (!tegra_vic_present_ready()) {
        *why = "VIC is not published";
        presentRefused(*why);
        return -19;
    }
    [[maybe_unused]] const uint64_t t0 = switch_now_us();
    TegraVICSurface surface;
    if (tegra_vic_surface_lookup(p->surfaceID, task, &surface)) {
        *why = "surface ID not found for this task";
        presentRefused(*why);
        return -22;
    }
    // Checked before the engine lock, at the windows' own addresses, so a refusal costs no engine time.
    Placement pl;
    TegraVICImage src;
    TegraVICImage dst;
    TegraVICOp op;
    *why = place(p, &surface.desc, &pl);
    TegraVICJob *check = *why ? nullptr : IONew(TegraVICJob, 1);
    if (!*why && !check)
        *why = "no memory for the check";
    if (!*why) {
        describe(p, pl, kTegraVICWindowBase + TEGRA_VIC_WINDOW_SRC * kTegraVICWindowBytes,
                 kTegraVICWindowBase + TEGRA_VIC_WINDOW_DST * kTegraVICWindowBytes, &src, &dst, &op);
        *why = tegra_vic_build(&src, &dst, &op, tegra_vic_traits(), tegra_vic_config_address(),
                               tegra_vic_filter_address(), check);
    }
    if (check)
        IODelete(check, TegraVICJob, 1);
    if (*why) {
        tegra_vic_surface_release(&surface);
        presentRefused(*why);
        return -95;
    }
    // The engine lock from here: the page list is shared by every decoder thread that presents.
    tegra_vic_lock();
    if (listDestination(&surface, pl) != kIOReturnSuccess) {
        tegra_vic_unlock();
        tegra_vic_surface_release(&surface);
        *why = "the surface could not be wired";
        presentRefused(*why);
        return -12;
    }
    const uint32_t srcPages = (uint32_t)((p->bytes + 0xfff) >> 12);
    const uint32_t dstPages = pl.pages[0] + pl.pages[1];
    const uint64_t dstBase = tegra_vic_window_map(TEGRA_VIC_WINDOW_DST, gPresentPages, dstPages, true);
    const uint64_t srcBase = tegra_vic_window_map(TEGRA_VIC_WINDOW_SRC, p->pages, srcPages, false);
    describe(p, pl, srcBase, dstBase, &src, &dst, &op);
    // NVDEC wrote the picture to DRAM and no CPU writes it (NVDEC's buffers are zeroed and cleaned when they are
    // handed out, and the decoder only reads them): the source needs no cache maintenance. The destination does: a
    // fresh IOSurface's pages were zeroed through the CPU's caches, and a dirty line written back later would land on
    // VIC's output (a green stripe); the second pass drops lines a CPU reader may have pulled in meanwhile.
    cacheRectangle(SWITCH_DCACHE_CLEAN_INVALIDATE, pl);
    *why = tegra_vic_build(&src, &dst, &op, tegra_vic_traits(), tegra_vic_config_address(), tegra_vic_filter_address(),
                           &gPresentJob);
    uint64_t busy = 0;
    const int err = *why ? -95 : tegra_vic_run(&gPresentJob, &busy);
    if (!*why && err)
        *why = "the job did not finish";
    cacheRectangle(SWITCH_DCACHE_CLEAN_INVALIDATE, pl);
    tegra_vic_window_clear(TEGRA_VIC_WINDOW_DST);
    tegra_vic_window_clear(TEGRA_VIC_WINDOW_SRC);
#if TEGRA_VIC_STATS
    gPresentStats.pictures++;
    if (err)
        gPresentStats.failed++;
    gPresentStats.engineUs += busy;
    gPresentStats.cpuUs += (switch_now_us() - t0) - busy;
    gPresentStats.bytes += ((uint64_t)srcPages + dstPages) << 12;
    presentStatsLine();
#endif
    tegra_vic_unlock();
    tegra_vic_surface_complete(&surface);
    tegra_vic_surface_release(&surface);
    return err;
}
