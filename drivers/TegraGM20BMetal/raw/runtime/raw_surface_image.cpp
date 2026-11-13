// SPDX-License-Identifier: MIT
// Device::createSurfaceImage (raw_gpu.h): a shared texture over the IOSurface every process opening it imports. The import
// carries the surface's ID, so the kernel keeps the surface's coherency across every process's locks, and never asks the
// kernel's SurfaceLayout, which decides a one-level 2D layout: a surface of no width is never one it tiles
// (SwitchGpuSurfaceGeometry.h switch_gpu_surface_packed), so the bytes stay in the image the descriptor gives.
#include "raw_lock.h"
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#include <cstring>
#include "raw_gpu.h"

namespace raw {

bool Device::createSurfaceImage(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                const SurfaceImage &image, uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    const fx::MipChain &c = image.layout;
    const uint64_t page = uint64_t(getpagesize());
    const uint64_t bytes = (allocBytes + page - 1) & ~(page - 1);
    if (!f || image.kind == SurfaceImage::Plain || !surfaceId || !surface || allocBytes < c.bytes ||
        (uintptr_t(base) & (page - 1)) || !shape(width, height, format, t))
        return false;
    Import *i = new Import;
    if (!k_.importPages(base, bytes, i->bo, uint32_t(surfaceId))) {
        delete i;
        return false;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!k_.bindImport(base, i->bo, TGM_LAYOUT_KIND)) {
        delete i;
        return false;
    }
    i->kind = TGM_LAYOUT_KIND;
    i->refs = 1;
    i->id = surfaceId;
    i->base = uintptr_t(base);
    i->bytes = bytes;
    i->owner = CFRetain(surface);
    RAW_DIAGNOSTIC(stats.imports++);
    RAW_DIAGNOSTIC(stats.importsMade++);
    RAW_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    if (!(t.tic = allocTic())) {
        release(i);
        return false;
    }
    t.import = i;
    t.va = i->bo.va;
    t.cpu = static_cast<uint8_t *>(base);
    t.storage = {t.va, c.bytes, false};
    if (image.kind == SurfaceImage::Blocks) {
        layBlocks(c, f, width, height, image.type, t);
        return true;
    }
    const fx::MipLevel &l0 = c.level[0];
    t.pitch = l0.rowBytes;
    t.blockLinear = true;
    t.blockHeight = l0.yLog2;
    t.type = image.type;
    switch (image.kind) {
    case SurfaceImage::Chain: {
        t.layerBytes = c.bytes;
        t.chain = new Texture::Chain;
        t.chain->layout = c;
        t.chain->va = t.va;
        t.chain->fmt2d = fx::twodFormat(format);
        uint32_t w[8];
        fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, c.levels, w);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        RAW_DIAGNOSTIC(stats.chains++);
        RAW_DIAGNOSTIC(stats.chainBytes += c.bytes);
        return true;
    }
    case SurfaceImage::Layers:
    case SurfaceImage::Volume:
        layImage(c, f, image.type, t);
        return true;
    case SurfaceImage::Samples:
        t.layerBytes = uint64_t(l0.rowBytes) * l0.rows;
        t.samples = c.samples;
        break;
    default:
        t.layerBytes = c.bytes;
        break;
    }
    writeTic(t);
    return true;
}

}  // namespace raw
