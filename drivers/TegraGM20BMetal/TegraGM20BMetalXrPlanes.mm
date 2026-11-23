// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalXrPlanes.h"
#include "format_standin.h"
#include "surface_refusal.h"
#include "xr_pass.h"
#include "xr_pools.h"

#include <algorithm>
#include <cstring>

static_assert(kXrPlanesOverflowTag >= gm20b::kFormatStandInTag + 2 * 8,
              "an overflow image's tag shares a fixed tag's memory");

bool xrPlanesStorable(MTLTextureDescriptor *d)
{
    const bool stores = d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderWrite);
    return !stores || (gRM->raw->xrPlanesImagePipeline(false) && gRM->raw->xrPlanesImagePipeline(true));
}

// A 2D texture of one level and sample, not turned
static bool planesForm(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return gm20b::twoPlaneXr(uint32_t(d.pixelFormat)) && xrPlanesStorable(d) && d.textureType == MTLTextureType2D &&
           d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned;
}

bool xrLastPlane(IOSurfaceRef s, NSUInteger plane)
{
    return plane + 1 == std::max<size_t>(IOSurfaceGetPlaneCount(s), 1);
}

size_t xrGpuRows(IOSurfaceRef s, NSUInteger plane)
{
    return (IOSurfaceGetPlaneCount(s) ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s)) &
           ~size_t(15);
}

bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    return planesForm(d) && plane + 1 < IOSurfaceGetPlaneCount(s);
}

bool xrWordPlaneShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    const bool writes =
        d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
    return planesForm(d) && (writes || !xrGpuRows(s, plane)) && xrLastPlane(s, plane);
}

// The planes at Metal's GPU layout, each offset rounded down to 16, the rows each at Metal's strides
// (createXrPlaneTexture), and plane `word`'s own rows kept for the CPU copies, which an M4 Max makes at the plane's layout
// (m2v-textures9 probes/xr.txt 5a-5n)
static bool holdInPlanes(SwitchMetalTexture *t, IOSurfaceRef s, uint64_t word, uint32_t wordPitch, uint64_t alpha,
                         uint32_t alphaPitch, uint32_t width, uint32_t height)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const char *why = "";
    if (!gRM->raw->createXrPlaneTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                        word & ~uint64_t(15), wordPitch, alpha & ~uint64_t(15), alphaPitch, width,
                                        height, uint32_t(d.pixelFormat), t->_raw, &why)) {
        gm20b::surfaceRefusal = why;
        return false;
    }
    if (((word | wordPitch) & 15) && !t->_raw.luma->blockLinear) {
        t->_cpuRows = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s)) + word;
        t->_cpuPitch = wordPitch;
    }
    if (!keepExtendedRange(d, t->_raw, false)) {
        gm20b::surfaceRefusal = "no memory or texture header for the stand-in it is sampled through";
        gRM->raw->destroyTexture(t->_raw);
        return false;
    }
    return true;
}

bool makeXrWordPlane(SwitchMetalTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height)
{
    const bool planes = IOSurfaceGetPlaneCount(s) > 0;
    const uint64_t word =
        planes ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, t->_plane)) - uintptr_t(IOSurfaceGetBaseAddress(s)) : 0;
    const size_t pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, t->_plane) : IOSurfaceGetBytesPerRow(s);
    return holdInPlanes(t, s, word, uint32_t(pitch), 0, 0, width, height);
}

bool makeXrPlanes(SwitchMetalTexture *t, IOSurfaceRef s, uint32_t width, uint32_t height)
{
    const uintptr_t at = uintptr_t(IOSurfaceGetBaseAddress(s));
    MTLTextureDescriptor *d = t->_descriptor;
    const NSUInteger p = t->_plane;
    const uint64_t word = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, p)) - at,
                   alpha = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, p + 1)) - at;
    if (!holdInPlanes(t, s, word, uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, p)), alpha,
                      uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, p + 1)), width, height))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        const gm20b::Texture &w = *t->_raw.luma, &a = *t->_raw.chroma;
        os_log(gm20bLog(),
               "SwitchMetal: two-plane XR on (%ux%u '%c%c%c%c', drawn in its planes: rows %u and %u, va&127 %llu and "
               "%llu, %s "
               "and %s)",
               unsigned(d.width), unsigned(d.height), char(IOSurfaceGetPixelFormat(s) >> 24),
               char(IOSurfaceGetPixelFormat(s) >> 16), char(IOSurfaceGetPixelFormat(s) >> 8),
               char(IOSurfaceGetPixelFormat(s)), w.pitch, a.pitch, (unsigned long long)(w.va & 127),
               (unsigned long long)(a.va & 127), w.renderable ? "direct" : "shadowed",
               a.renderable ? "direct" : "shadowed");
    }
    return true;
}

uint32_t xrWordsFormat(uint32_t format)
{
    constexpr uint32_t kBgr10Xr = 554, kBgr10XrSrgb = 555;
    return format == gm20b::kRgb10A8XrSrgb ? kBgr10XrSrgb : kBgr10Xr;
}

bool xrWordsTexture(SwitchMetalTexture *t)
{
    return t->_surface && t->_gm20bOk && gm20b::twoPlaneXr(uint32_t(t->_descriptor.pixelFormat)) && !t->_raw.xrPlanes;
}

void readAlphaZero(gm20b::Texture &standIn)
{
    constexpr uint8_t kNoAlpha[4] = {2, 3, 4, 0};  // MTLTextureSwizzle Red, Green, Blue, Zero
    gRM->raw->swizzle(standIn, kNoAlpha);
}

bool xrPlaneWords(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes,
                  NSUInteger row, bool write)
{
    const gm20b::Texture *w = t->_gm20bOk && t->_raw.xrPlanes ? t->_raw.luma : nullptr;
    if (!w || !w->cpu || level || slice || r.origin.z || r.size.depth != 1 || r.origin.x + r.size.width > w->width ||
        r.origin.y + r.size.height > w->height)
        return false;
    gm20b::HostAccess access;
    if (w->blockLinear) {  // the kernel's layout of the surface (cpuTiled)
        if (!gRM->raw->hostAccess(*w, write, access))
            [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
        const tgm_layout &l = w->import->layout;
        const uint32_t x = uint32_t(r.origin.x) + w->surfaceX, y = uint32_t(r.origin.y) + w->surfaceY;
        if (write)
            tgm_layout_write(&l, w->cpu, x, y, uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
        else
            tgm_layout_read(&l, w->cpu, x, y, uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
        return true;
    }
    uint8_t *const rows = t->_cpuRows ? t->_cpuRows : w->cpu;
    const size_t pitch = t->_cpuRows ? t->_cpuPitch : w->pitch;
    const size_t line = r.size.width * gm20b::bufferTexelBytes(t->_raw.format, t->_raw.bpp);
    uint8_t *first = rows + r.origin.y * pitch + r.origin.x * w->bpp;
    if (!gRM->raw->hostAccess(*w, first, (r.size.height - 1) * pitch + line, write, access))
        [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *words = first + y * pitch;
        if (write)
            memcpy(words, bytes + y * row, line);
        else
            memcpy(bytes + y * row, words, line);
    }
    return true;
}

static int planesPass(gm20b::CommandBuffer *rc, const gm20b::PassDesc &p, gm20b::Pipeline *state,
                      const gm20b::Texture &read0, const gm20b::Texture *read1, uint32_t width, uint32_t height,
                      const char **why)
{
    gm20b::RenderEncoder *e = rc->render(p);
    if (!e) {
        *why = "a two-plane RGB10A8_2P_XR10 texture's image or planes the driver cannot render";
        return 1;
    }
    e->setPipeline(state);
    e->setTexture(0, &read0);
    if (read1)
        e->setTexture(1, read1);
    e->setViewport(0, 0, width, height, 0, 1);
    e->setScissor(0, 0, width, height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return 2;
    e->end();
    return 0;
}

int mergeXrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, uint32_t k, gm20b::Texture *&image,
                  const char **why)
{
    constexpr uint32_t kRG32Uint = 103;
    image = rc->turnScratch(kRG32Uint, t.width, t.height, xrPlanesImageTag(k));
    gm20b::Pipeline *merge = gRM->raw->xrPlanesImagePipeline(false);
    if (!image || !merge) {
        *why = !image ? "no image for a written two-plane RGB10A8_2P_XR10 texture"
                      : "the pack has no state merging a two-plane RGB10A8_2P_XR10 texture's planes";
        return 1;
    }
    gm20b::fillXrPools(*rc, t);
    gm20b::PassDesc p;
    p.color[0].texture = image;
    p.color[0].load = gm20b::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    return planesPass(rc, p, merge, *t.luma, t.chroma, t.width, t.height, why);
}

int splitXrPlanes(gm20b::CommandBuffer *rc, const gm20b::Texture &t, const gm20b::Texture &image, bool cleared,
                  const char **why)
{
    gm20b::Pipeline *split = gRM->raw->xrPlanesImagePipeline(true);
    if (!split) {
        *why = "the pack has no state splitting a two-plane RGB10A8_2P_XR10 texture's image";
        return 1;
    }
    const bool pools = gm20b::xrPlanesOverlap(t);
    gm20b::PassDesc p;
    p.color[0].texture = pools ? t.luma->sampled : t.luma;
    p.color[1].texture = pools && t.chroma->sampled ? t.chroma->sampled : t.chroma;
    for (gm20b::ColorAttachment &a : p.color) {
        a.load = gm20b::ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = 2;
    if (const int failed = planesPass(rc, p, split, image, nullptr, t.width, t.height, why))
        return failed;
    return pools ? gm20b::storeXrPlaneRows(*gRM->raw, *rc, t, cleared, why) : 0;
}
