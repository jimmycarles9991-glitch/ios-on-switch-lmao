// SPDX-License-Identifier: MIT
// See gpu.h (YCbCr and packed 10-bit textures over a surface's planes: the images their conversions draw).
#include "lock.h"
#include "gpu.h"

#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// A one-plane 4:2:2 YCbCr texture (YcbcrPlan::onePlane): its image and the luma and chroma pool images its conversion reads,
// which BlitEncoder::splitPlane fills from the surface's plane
bool Device::createOnePlaneYcbcr(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                 const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t)
{
    const uint64_t mapped = (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    const uint64_t pairs = plan.chromaWidth;
    uint32_t imageWidth = width, imageHeight = height;
    if (plan.buffer)
        ycbcrBufferExtent(width, imageWidth, imageHeight);
    if (!pairs || !height || plan.lumaPitch < 16 || (plan.lumaPitch & 15) || (plan.luma & 15) ||
        plan.luma + uint64_t(plan.lumaPitch) * (imageHeight - 1) + 4 * pairs >
            (plan.window ? plan.readable : std::min(plan.readable, mapped)) ||
        (uintptr_t(base) & (pageSize() - 1)))
        return false;
    Import *planes;
    {
        std::unique_lock<gm20b::Mutex> l(lock_);
        planes = importSurface(l, surfaceId, base, mapped, surface);
    }
    if (!planes)
        return false;
    Texture *y = new Texture, *c = new Texture;
    if (!createTexture(plan.lumaWidth, imageHeight, plan.lumaFormat, *y) ||
        !createTexture(plan.chromaWidth, plan.chromaHeight, plan.chromaFormat, *c) || y->blockLinear ||
        c->blockLinear || !createTexture(imageWidth, imageHeight, plan.image, t, false, !plan.buffer)) {
        for (Texture *x : {y, c}) {
            destroyTexture(*x);
            delete x;
        }
        DeferredFrees deferred(*this);
        std::lock_guard<gm20b::Mutex> l(lock_);
        release(planes, deferred);
        return false;
    }
    t.import = planes;
    t.planes = {planes->bo.va + plan.luma, 0, plan.lumaPitch, 0};
    t.onePlane = true;
    t.pairsReversed = plan.pairsReversed;
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    if (plan.buffer) {
        t.type = fx::kTexOneDBuffer;
        t.width = t.texelWidth = width;
        t.height = t.texelHeight = 1;
    }
    ycbcrImageHeader(t, plan.output, plan.shape[2]);
    if (plan.window && !openSparseWindow(t, plan.readable)) {
        destroyTexture(t);
        return false;
    }
    return true;
}

bool Device::createYcbcrTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t)
{
    if (!ycbcrPipeline(plan.output))
        return false;
    if (plan.onePlane)
        return createOnePlaneYcbcr(surfaceId, base, allocBytes, surface, plan, width, height, t);
    Texture *y = new Texture, *c = plan.chromaWidth ? new Texture : nullptr;
    auto undo = [&] {
        for (Texture *x : {y, c})
            if (x) {
                destroyTexture(*x);
                delete x;
            }
        return false;
    };
    uint32_t imageWidth = width, imageHeight = height;
    if (plan.buffer)
        ycbcrBufferExtent(width, imageWidth, imageHeight);
    // The kernel decides each plane's layout from the ask. It lays out no surface of planes block-linear
    // (SwitchGpuSurfaceGeometry.h switch_gpu_surface_packed), as a re-laid plane would move the other plane's bytes and
    // rows out from under the CPU and the video engines; a planeless surface read as luma alone may come back
    // block-linear, as any surface of its shape does.
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.luma, plan.lumaWidth, imageHeight,
                              plan.lumaPitch, plan.lumaFormat, *y, false, plan.readable, plan.meet))
        return undo();
    if (c &&
        !createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.chroma, plan.chromaWidth, plan.chromaHeight,
                              plan.chromaPitch, plan.chromaFormat, *c, false, plan.readable, plan.meet))
        return undo();
    constexpr uint8_t kGreenBlue[4] = {3, 4, 0, 1};  // MTLTextureSwizzle Green, Blue, Zero, One
    if (c && plan.chromaGreenBlue)
        swizzle(c->sampled ? *c->sampled : *c, kGreenBlue);
    // block-linear, as NVK makes m2v's converted image; a texture buffer's rows linear, back to back (YcbcrPlan::buffer)
    const bool image = plan.levels > 1 ? createMipTexture(width, height, plan.image, plan.levels, t)
                                       : createTexture(imageWidth, imageHeight, plan.image, t, false, !plan.buffer);
    if (!image)
        return undo();
    if (plan.nativePlanes) {
        y->nativePlane = 0;
        if (c)
            c->nativePlane = 1;
    }
    y->storePitch = plan.lumaStorePitch;
    if (c)
        c->storePitch = plan.chromaStorePitch;
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    t.levelZero = plan.levels > 1;
    if (plan.buffer) {
        t.type = fx::kTexOneDBuffer;
        t.width = t.texelWidth = width;
        t.height = t.texelHeight = 1;
    }
    ycbcrImageHeader(t, plan.output, plan.shape[2]);
    if (plan.window && !openSparseWindow(t, plan.readable)) {
        destroyTexture(t);
        return false;
    }
    return true;
}

void Device::ycbcrImageHeader(Texture &t, uint8_t output, uint32_t packedCodes)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (t.type == fx::kTexOneDBuffer)  // the image's rows under a texel buffer's header
        writeTic(t);
    uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
    if (ycbcrLumaFirst(output)) {
        constexpr uint8_t kLumaFirst[4] = {3, 4, 2, 5};  // the codes image's green, blue, red: Y, Cb, Cr
        w[0] = swizzledWord0(w[0], kLumaFirst);
    }
    // its alpha read as one, as m2v's view of its own converted image reads it (W_SOURCE ONE_FLOAT; the conversion writes 1
    // there too: the same texels, now the same header)
    w[0] = (w[0] & ~(7u << 28)) | 7u << 28;
    // a packed plane's channels past its codes read 0, as Metal reads R10Unorm_PACKED and RG10Unorm_PACKED (Z_SOURCE, and
    // Y_SOURCE of a one-code texel, ZERO)
    if (output == YcbcrPackedPlane)
        w[0] &= ~(7u << 25 | (packedCodes == 1 ? 7u << 22 : 0u));
    noteTicWrite();
}

}  // namespace gm20b
