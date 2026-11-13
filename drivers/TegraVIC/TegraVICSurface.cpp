// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOSurfaces for the scaler, through Apple's IOSurface kext (com.apple.iokit.IOSurface, prelinked): an ID becomes a
// retained IOSurface in the calling task's namespace, its geometry is read with the kext's own accessors, and its pages
// are wired for the engine with IOSurface::prepare/complete, as AppleM2ScalerCSCDriver does (CONTRACT.md).
//
// The accessors are bound by address (original-bindings.S, checked against the kernelcache on every build by
// abi_check.py: the slid address of each one holds the instructions read when it was identified):
//   IOSurfaceRoot::lookupSurface(id, task)  find_surface for the task (its own surfaces, those its IOSurfaceRootUserClient
//                                            holds, global ones), then a try-retain; release with OSObject::release
//   getMemoryDescriptor, getPixelFormat, getWidth, getHeight, getBytesPerRow, getPlaneCount, getAllocSize,
//   getProtectionOptions; getPlaneWidth/Height/Offset/BytesPerRow(plane) (plane 0 falls back to the surface's own
//   fields when the surface has no plane array); prepare/complete (wire count, IOMemoryDescriptor::prepare).
// The IOSurfaceRoot is the service named "IOCoreSurfaceRoot".
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <string.h>
#include "TegraVIC.h"
#include "TegraVICSurface.h"
#include "SwitchVICSurfaceRange.h"
#include "OriginalIOSurface.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

extern "C" {
OSObject *switch_surface_copy_value(OSObject *surface, const OSSymbol *key, uint32_t *seed);
OSObject *tegra_vic_ios_lookup(IOService *root, uint32_t id, task_t task);
IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *surface);
uint32_t tegra_vic_ios_format(OSObject *surface);
uint64_t tegra_vic_ios_width(OSObject *surface);
uint64_t tegra_vic_ios_height(OSObject *surface);
uint64_t tegra_vic_ios_bytes_per_row(OSObject *surface);
uint32_t tegra_vic_ios_plane_count(OSObject *surface);
uint64_t tegra_vic_ios_alloc_size(OSObject *surface);
uint64_t tegra_vic_ios_protection(OSObject *surface);
uint64_t tegra_vic_ios_plane_width(OSObject *surface, uint32_t plane);
uint64_t tegra_vic_ios_plane_height(OSObject *surface, uint32_t plane);
uint64_t tegra_vic_ios_plane_offset(OSObject *surface, uint32_t plane);
uint64_t tegra_vic_ios_plane_bytes_per_row(OSObject *surface, uint32_t plane);
int tegra_vic_ios_prepare(OSObject *surface);
int tegra_vic_ios_complete(OSObject *surface);
// The end of a device's write (options 0) or read (1): a write moves the surface's seed (abi_check.py FIELDS).
int tegra_vic_ios_device_unlock(OSObject *surface, uint32_t options);
}

namespace {
IOService *gRoot;
const OSSymbol *gMatrixKey;
const OSSymbol *gTransferKey, *gPrimariesKey, *gChromaTopKey, *gChromaBottomKey;

uint32_t colorTag(OSObject *surface, const OSSymbol *key, uint32_t (*parse)(const char *))
{
    if (!key)
        return UINT32_MAX;
    uint32_t seed = 0;
    OSObject *value = switch_surface_copy_value(surface, key, &seed);
    auto *name = OSDynamicCast(OSString, value);
    const uint32_t result = value && !name ? UINT32_MAX : parse(name ? name->getCStringNoCopy() : nullptr);
    OSSafeReleaseNULL(value);
    return result;
}

uint32_t colorMatrix(OSObject *surface)
{
    if (!gMatrixKey)
        return SWITCH_VIC_MATRIX_UNSUPPORTED;
    uint32_t seed = 0;
    OSObject *value = switch_surface_copy_value(surface, gMatrixKey, &seed);
    auto *name = OSDynamicCast(OSString, value);
    const uint32_t matrix = value && !name ? uint32_t(SWITCH_VIC_MATRIX_UNSUPPORTED)
                                           : switch_vic_matrix_tag(name ? name->getCStringNoCopy() : nullptr);
    OSSafeReleaseNULL(value);
    return matrix;
}
}

extern "C" uint32_t switch_vic_surface_matrix(OSObject *surface)
{
    return surface ? colorMatrix(surface) : uint32_t(SWITCH_VIC_MATRIX_UNSUPPORTED);
}

extern "C" void switch_vic_surface_color(OSObject *surface, TegraVICSurfaceDesc *desc)
{
    desc->colorMatrix = colorMatrix(surface);
    desc->transfer = colorTag(surface, gTransferKey, switch_surface_transfer_tag);
    desc->primaries = colorTag(surface, gPrimariesKey, switch_surface_primaries_tag);
    desc->chromaTop = colorTag(surface, gChromaTopKey, switch_surface_chroma_tag);
    desc->chromaBottom = colorTag(surface, gChromaBottomKey, switch_surface_chroma_tag);
}

extern "C" bool tegra_vic_surface_start(void)
{
    if (gRoot)
        return true;
    OSDictionary *match = IOService::nameMatching("IOCoreSurfaceRoot");
    if (!match)
        return false;
    IOService *root = IOService::waitForMatchingService(match, 5ULL * 1000 * 1000 * 1000);
    match->release();
    if (root) {
        gMatrixKey = OSSymbol::withCStringNoCopy("IOSurfaceYCbCrMatrix");
        gTransferKey = OSSymbol::withCStringNoCopy("IOSurfaceTransferFunction");
        gPrimariesKey = OSSymbol::withCStringNoCopy("IOSurfaceColorPrimaries");
        gChromaTopKey = OSSymbol::withCStringNoCopy("IOSurfaceChromaLocationTopField");
        gChromaBottomKey = OSSymbol::withCStringNoCopy("IOSurfaceChromaLocationBottomField");
        if (!gMatrixKey || !gTransferKey || !gPrimariesKey || !gChromaTopKey || !gChromaBottomKey) {
            OSSafeReleaseNULL(gMatrixKey);
            OSSafeReleaseNULL(gTransferKey);
            OSSafeReleaseNULL(gPrimariesKey);
            OSSafeReleaseNULL(gChromaTopKey);
            OSSafeReleaseNULL(gChromaBottomKey);
            root->release();
            return false;
        }
    }
    gRoot = root;  // kept (waitForMatchingService retained it)
    return gRoot != nullptr;
}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t id, task_t task, TegraVICSurface *out)
{
    bzero(out, sizeof(*out));
    if (!gRoot || !id)
        return kIOReturnBadArgument;
    OSObject *surface = tegra_vic_ios_lookup(gRoot, id, task);
    if (!surface)
        return kIOReturnBadArgument;  // as AppleM2ScalerCSCDriver: not this task's surface
    const IOReturn result = switch_vic_surface_from_object(surface, id, out);
    surface->release();
    return result;
}

extern "C" IOReturn switch_vic_surface_from_object(OSObject *surface, uint32_t id, TegraVICSurface *out)
{
    if (!out)
        return kIOReturnBadArgument;
    bzero(out, sizeof(*out));
    if (!surface)
        return kIOReturnBadArgument;
    surface->retain();
    out->surface = surface;
    out->id = id;
    out->memory = tegra_vic_ios_memory(surface);
    TegraVICSurfaceDesc *d = &out->desc;
    d->fourcc = tegra_vic_ios_format(surface);
    d->width = (uint32_t)tegra_vic_ios_width(surface);
    d->height = (uint32_t)tegra_vic_ios_height(surface);
    d->planes = tegra_vic_ios_plane_count(surface);
    switch_vic_surface_color(surface, d);
    const uint32_t planes = d->planes ? d->planes : 1;
    for (uint32_t p = 0; p < planes && p < TEGRA_VIC_MAX_PLANES; p++) {
        d->planeOffset[p] = tegra_vic_ios_plane_offset(surface, p);
        d->planeBytesPerRow[p] = tegra_vic_ios_plane_bytes_per_row(surface, p);
        d->planeWidth[p] = tegra_vic_ios_plane_width(surface, p);
        d->planeHeight[p] = tegra_vic_ios_plane_height(surface, p);
    }
    if (!d->planes) {
        // A surface without planes: its own fields (the plane-0 accessors already fall back to them).
        d->planeBytesPerRow[0] = tegra_vic_ios_bytes_per_row(surface);
        d->planeHeight[0] = d->height;
    }
    memcpy(&out->baseOffset, reinterpret_cast<const uint8_t *>(surface) + OriginalIOSurface::baseOffset,
           sizeof(out->baseOffset));
    d->length = tegra_vic_ios_alloc_size(surface);
    d->protection = tegra_vic_ios_protection(surface);
    if (!out->memory || !switch_vic_surface_extent_valid(out->memory->getLength(), out->baseOffset, d->length)) {
        tegra_vic_surface_release(out);
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *s)
{
    if (s->prepared)
        tegra_vic_surface_complete(s);
    if (s->surface)
        s->surface->release();  // IOSurface::release, as lookupSurface's caller must
    bzero(s, sizeof(*s));
}

// Wires the surface once (IOSurface::prepare) and lists the physical pages of [offset, offset + bytes), from the page
// holding `offset`. The memory must start on a page (IOSurface allocations do).
extern "C" IOReturn tegra_vic_surface_pages(TegraVICSurface *s, uint64_t offset, uint64_t bytes, uint64_t *pages,
                                            uint32_t max, uint32_t *count)
{
    *count = 0;
    if (!switch_vic_surface_subrange(s->desc.length, offset, bytes))
        return kIOReturnBadArgument;
    const uint64_t first = offset >> 12;
    const uint64_t end = (offset + bytes + 0xfff) >> 12;
    if (end - first > max)
        return kIOReturnBadArgument;
    if (!s->prepared) {
        // IOSurface::prepare keeps the surface's own wire count (and returns 0 even when the descriptor's prepare
        // fails); the descriptor is prepared once more here, which is the result that counts.
        if (tegra_vic_ios_prepare(s->surface) != kIOReturnSuccess)
            return kIOReturnNoMemory;
        if (s->memory->prepare() != kIOReturnSuccess) {
            (void)tegra_vic_ios_complete(s->surface);
            return kIOReturnNoMemory;
        }
        s->prepared = true;
    }
    if (!switch_dma_page_list(s->memory, s->baseOffset + offset, bytes, pages, max, count)) {
        tegra_vic_surface_complete(s);
        return kIOReturnNotAligned;
    }
    return kIOReturnSuccess;
}

extern "C" IOReturn tegra_vic_surface_prepare(TegraVICSurface *s, uint64_t extent, uint64_t *pages, uint32_t max,
                                              uint32_t *count)
{
    return tegra_vic_surface_pages(s, 0, extent, pages, max, count);
}

// r18-planes: VIC wrote the surface (a TransformSurface destination): its seed moves as a CPU write-unlock or Apple's
// scaler's frame-done moves it (AppleM2ScalerCSCDriver calls the same function on its destination), so whoever caches by
// seed (the raw GPU path's 4:2:0 conversions, IOSurfaceGetSeed) sees the new picture. Thread context; the surface's own
// lock is taken inside. Its next CPU lock cleans the CPU's caches once, as after Apple's engines.
extern "C" void tegra_vic_surface_written(const TegraVICSurface *s)
{
    if (s->surface)
        (void)tegra_vic_ios_device_unlock(s->surface, 0);
}

extern "C" void tegra_vic_surface_complete(TegraVICSurface *s)
{
    if (!s->prepared || !s->memory)
        return;
    s->memory->complete();
    (void)tegra_vic_ios_complete(s->surface);
    s->prepared = false;
}
