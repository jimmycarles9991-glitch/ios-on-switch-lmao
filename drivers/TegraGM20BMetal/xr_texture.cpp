// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 and RGB10A8_2P_XR10_sRGB textures held in Metal's two planes (xr_pass.h): the texture's own
// Texture has no memory, its planes are luma (the words, R32Uint) and chroma (the alpha bytes, R8Uint), each read
// through its pool image where its rows are off 32 bytes or overlap (xr_pools.h). Words over a surface the kernel
// laid out block-linear for an earlier texture are read and stored in that layout (an M4 Max makes 550 over a planeless
// 'w30r' a BGR10_XR target was cleared in, reads its words with alpha 0 and stores them with bits 30 and 31 clear). One
// written over a plane alone has no alpha plane: chroma is a memoryless target, its header (the pair's second) over a
// sparse range of the texture's own (Texture::window), which the GMMU reads as 0 and whose stores it drops, as an M4 Max
// reads alpha 0 there and stores none (m2v-textures9 probes/xr.txt 6a-6c). A plane's rows are read at Metal's GPU stride
// (ycbcr.h ycbcrPlaneStride: rows under 16 bytes 1 MiB apart) and stored at one of their own (planeStoreStride): an
// M4 Max stores rows under 16 bytes 16 apart, a word that lands on the alpha plane taking its byte as a pass orders them
// (m2v-textures11 probes/h1/rows550.txt).
#include "xr_pass.h"
#include "xr_pools.h"
#include "ycbcr.h"

#include "gpu.h"
#include "gm20b_gpu_internal.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gm20b {

// MTLTextureSwizzle: a header of plane 0 as RGB10A2Unorm reads the red code in its blue component (xr.h: red at bit 20)
constexpr uint8_t kWordChannels[4] = {4, 3, 2, 1};  // Blue, Green, Red, One
constexpr uint32_t kRGB10A2Unorm = 90, kR8Unorm = 10;

bool Device::createXrPlaneTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t word, uint32_t wordRows, uint64_t alpha, uint32_t alphaRows, uint32_t width,
                                  uint32_t height, uint32_t format, Texture &t, const char **why)
{
    Texture *w = new Texture, *a = new Texture;
    const auto undo = [&](const char *reason) {
        t.luma = t.chroma = nullptr;
        for (Texture *x : {w, a}) {
            destroyTexture(*x);
            delete x;
        }
        *why = reason;
        return false;
    };
    if (!twoPlaneXr(format) || !shape(width, height, format, t) || !wordRows)
        return undo("no two-plane extended-range texture of its format and size");
    const bool alone = !alphaRows;
    const uint32_t wordPitch = ycbcrPlaneStride(wordRows), alphaPitch = alone ? 0 : ycbcrPlaneStride(alphaRows);
    // rows read at their pitch past their row's or their plane's end (xr_pools.h) are imported to the bytes they
    // reach: to the surface's last page, past it through a sparse window (reads 0, stores dropped, as an M4 Max reads 0
    // past a surface's last page where nothing else is mapped)
    const uint64_t page = pageSize(),
                   mapped = ((uintptr_t(base) + allocBytes + page - 1) & ~(page - 1)) - uintptr_t(base);
    const uint64_t end = std::max(word + uint64_t(wordPitch) * (height - 1) + 4ull * width,
                                  alone ? 0 : alpha + uint64_t(alphaPitch) * (height - 1) + width);
    const bool reaches = wordPitch < 4 * width || (!alone && alphaPitch < width) || end > allocBytes;
    const bool window = end > mapped;
    const uint64_t readable = !reaches ? 0 : window ? end : mapped;
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, word, width, height, wordPitch, kXrWordFormat, *w,
                              false, readable) ||
        (alone ? !createMemoryless(width, height, kXrAlphaFormat, *a)
               : !createSurfaceTexture(surfaceId, base, allocBytes, surface, alpha, width, height, alphaPitch,
                                       kXrAlphaFormat, *a, false, readable)))
        return undo("a plane the driver cannot import");
    // the kernel tiles only a surface of no planes (SwitchGpuSurfaceGeometry.h switch_gpu_surface_packed), whose words
    // have no alpha plane
    if (a->blockLinear || !(w->renderable || w->shadowTarget) || !(a->renderable || a->shadowTarget))
        return undo("a plane the driver cannot render, or an alpha plane laid out block-linear");
    // stored through both pools (xr_pools.h xrPlanesOverlap): a plane whose rows do not overlap takes one too
    const auto pooled = [&](Texture &plane) {
        if (plane.sampled || plane.memoryless)
            return true;
        Texture *image = new Texture;
        if (!createTexture(width, height, plane.format, *image, false, true)) {
            delete image;
            return false;
        }
        std::lock_guard<gm20b::Mutex> l(lock_);
        freeTic(plane.tic);
        plane.sampled = image;
        plane.tic = image->tic;
        return true;
    };
    if (planeStoreStride(wordRows) != wordPitch)
        w->storePitch = planeStoreStride(wordRows);
    if (!alone && planeStoreStride(alphaRows) != alphaPitch)
        a->storePitch = planeStoreStride(alphaRows);
    const Texture ws = storedRows(*w, t), as = storedRows(*a, t);
    if ((w->storePitch || a->storePitch || overlapsRows(ws) ||
         (!alone && (overlapsRows(as) || xrPlanesCross(ws, as)))) &&
        (!pooled(*w) || !pooled(*a)))
        return undo("no memory for the pool images planes that overlap are read and stored through");
    // a plane alone's alpha sink: a sparse range of its own, or the end of the window its words' rows past the last page
    // are read through
    const uint32_t sinkPitch = (width + 31) & ~31u;
    const uint64_t gpuPage = k_.info().page_size, sinkBytes = uint64_t(sinkPitch) * height,
                   sinkSpan = (sinkBytes + gpuPage - 1) & ~(gpuPage - 1);
    if (alone && !window && !openSparseSink(t, sinkBytes))
        return undo("no sparse range for the alpha it keeps none of (a kernel without TegraGM20BInfoSparse, or no VA)");
    t.luma = w;
    t.chroma = a;
    if (window && !openSparseWindow(t, end, alone ? sinkBytes : 0))
        return undo("rows past its surface's last page with no sparse window (a kernel without "
                    "TegraGM20BInfoSparse, or no VA)");
    uint32_t pair = 0;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        if ((pair = allocTicPair())) {
            Texture word0 = w->sampled ? *w->sampled : *w, alpha0 = a->sampled ? *a->sampled : *a;
            word0.format = kRGB10A2Unorm;
            memcpy(word0.swizzle, kWordChannels, sizeof word0.swizzle);
            word0.tic = pair;
            alpha0.format = kR8Unorm;
            alpha0.tic = pair + 1;
            writeTic(word0);
            if (alone) {
                alpha0.va = window ? t.window.va + t.window.bytes - sinkSpan : t.window.va;
                alpha0.pitch = sinkPitch;
                alpha0.memoryless = alpha0.blockLinear = false;
            }
            writeTic(alpha0);
        }
    }
    if (!pair) {
        closeSparseWindow(t);
        return undo("no room for its headers");
    }
    if (alone)  // freed with the pair (destroyTexture)
        a->tic = pair + 1;
    t.tic = pair;
    t.xrPlanes = true;
    w->xrPlane = 1;
    a->xrPlane = 2;
    t.luma = w;
    t.chroma = a;
    t.cpu = nullptr;  // its CPU copies move plane 0's words in the surface (TegraGM20BMetalXrPlanes.mm xrPlaneWords)
    return true;
}

bool Device::createXrMemoryless(Texture &t)
{
    Texture *w = new Texture, *a = new Texture;
    if (!createMemoryless(t.width, t.height, kXrWordFormat, *w) ||
        !createMemoryless(t.width, t.height, kXrAlphaFormat, *a)) {
        delete w;
        delete a;
        return false;
    }
    t.xrPlanes = true;
    w->xrPlane = 1;
    a->xrPlane = 2;
    t.luma = w;
    t.chroma = a;
    return true;
}

}  // namespace gm20b
