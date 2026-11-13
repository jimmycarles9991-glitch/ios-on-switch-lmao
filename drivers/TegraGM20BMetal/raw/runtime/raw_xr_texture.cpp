// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 textures held in Metal's two planes (raw_xr_pass.h): the texture's own Texture has no memory, its planes are
// luma (the words, R32Uint) and chroma (the alpha bytes, R8Uint).
#include "raw_xr_pass.h"

#include "raw_gpu.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace raw {

// MTLTextureSwizzle: a header of plane 0 as RGB10A2Unorm reads the red code in its blue component (raw_xr.h: red at bit 20)
constexpr uint8_t kWordChannels[4] = {4, 3, 2, 1};  // Blue, Green, Red, One
constexpr uint32_t kRGB10A2Unorm = 90, kR8Unorm = 10;

bool Device::createXrPlaneTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t word, uint32_t wordPitch, uint64_t alpha, uint32_t alphaPitch,
                                  uint32_t width, uint32_t height, Texture &t, const char **why)
{
    Texture *w = new Texture, *a = new Texture;
    const auto undo = [&](const char *reason) {
        for (Texture *x : {w, a}) {
            destroyTexture(*x);
            delete x;
        }
        *why = reason;
        return false;
    };
    if (!shape(width, height, 550, t))
        return undo("no 550 texture of its size");
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, word, width, height, wordPitch, kXrWordFormat,
                              *w) ||
        !createSurfaceTexture(surfaceId, base, allocBytes, surface, alpha, width, height, alphaPitch, kXrAlphaFormat,
                              *a))
        return undo("a plane the raw path cannot import");
    if (w->blockLinear || a->blockLinear || w->sampled || a->sampled || !(w->renderable || w->shadowTarget) ||
        !(a->renderable || a->shadowTarget))
        return undo("a plane tiled, or of rows or an offset off 32 bytes");
    uint32_t pair = 0;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        if ((pair = allocTicPair())) {
            Texture word0 = *w, alpha0 = *a;
            word0.format = kRGB10A2Unorm;
            memcpy(word0.swizzle, kWordChannels, sizeof word0.swizzle);
            word0.tic = pair;
            alpha0.format = kR8Unorm;
            alpha0.tic = pair + 1;
            writeTic(word0);
            writeTic(alpha0);
        }
    }
    if (!pair)
        return undo("no room for its headers");
    t.tic = pair;
    t.xrPlanes = true;
    w->xrPlane = 1;
    a->xrPlane = 2;
    t.luma = w;
    t.chroma = a;
    t.cpu = nullptr;  // Metal's CPU access reads and writes the planes: the surface's texture on m2v
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

const char *xrPlanesPass(const Texture *const color[8], bool &planes)
{
    planes = false;
    uint32_t formats[8] = {};
    for (uint32_t j = 0; j < 8; j++)
        if (const Texture *t = color[j]) {
            formats[j] = t->format;
            planes |= t->xrPlanes && !t->memoryless;
        }
    if (!planes)
        return nullptr;
    const std::array<int8_t, 8> alpha = xrAlphaSlots(formats);
    for (uint32_t j = 0; j < 8; j++) {
        if (xrForm(formats[j]) != XrForm::Planes)
            continue;
        if (!color[j]->xrPlanes)
            return "an RGB10A8_2P_XR10 texture of one image beside one held in its two planes";
        if (alpha[j] < 0)
            return "no free colour index for an RGB10A8_2P_XR10 attachment's alpha plane";
    }
    return nullptr;
}

const char *expandXrPlanes(PassDesc &p, Device &d)
{
    const Texture *color[8] = {};
    uint32_t formats[8] = {};
    for (uint32_t j = 0; j < p.count && j < 8; j++)
        if ((color[j] = p.color[j].texture))
            formats[j] = color[j]->format;
    bool planes = false;
    if (const char *refused = xrPlanesPass(color, planes); refused || !planes)
        return refused;
    const std::array<int8_t, 8> alpha = xrAlphaSlots(formats);
    for (uint32_t j = 0; j < 8; j++) {
        if (alpha[j] < 0)
            continue;
        const ColorAttachment a = p.color[j];
        const std::array<uint32_t, 2> clears = xrPlaneClears(a.clear);
        ColorAttachment &word = p.color[j], &byte = p.color[alpha[j]];
        word = byte = a;
        word.texture = a.texture->luma;
        byte.texture = a.texture->chroma;
        for (ColorAttachment *plane : {&word, &byte}) {
            const uint32_t bits = plane == &word ? clears[0] : clears[1];
            memset(plane->clear, 0, sizeof plane->clear);
            memcpy(&plane->clear[0], &bits, 4);
        }
        if (p.storageMask >> j & 1)
            p.storageMask |= uint8_t(1u << alpha[j]);
        p.count = std::max<uint32_t>(p.count, uint32_t(alpha[j]) + 1);
    }
    RAW_DIAGNOSTIC(d.stats.xrPlanePasses++);
    return nullptr;
}

}  // namespace raw
