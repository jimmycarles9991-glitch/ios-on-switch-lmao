// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalXrCopies.h"
#import "TegraGM20BMetalBlitImages.h"
#include "blit_planes.h"
#include "xr.h"
#include "xr_copy.h"

#include <array>

namespace {

bool descriptorXr(SwitchMetalTexture *t)
{
    return t->_gm20bOk && !t->_surface && !t->_raw.xrPlanes && t->_raw.bpp == 8 &&
           gm20b::twoPlaneXr(uint32_t(t->_descriptor.pixelFormat));
}

bool surfaceXr(SwitchMetalTexture *t)
{
    return t->_gm20bOk && t->_raw.xrPlanes && t->_raw.luma && !t->_raw.luma->blockLinear &&
           gm20b::twoPlaneXr(uint32_t(t->_descriptor.pixelFormat));
}

// One side of a copy: a descriptor's level-0 image from the copy's slice on (an M4 Max moves level 0 whatever level a
// copy names: probes/c1copies/RESULT.txt MIPS), or a surface's word plane and alpha plane at plane 0's rows; each plane
// as xr_copy.h addresses it
struct XrSide {
    bool surface = false;
    uint32_t slice = 0;
    gm20b::Texture image, words, alpha;
    std::array<gm20b::XrCopySide, 2> plane;
};

bool xrSide(SwitchMetalTexture *t, uint32_t slice, bool source, XrSide &out)
{
    out.surface = surfaceXr(t);
    if (!out.surface) {
        if (!blitImage(t, 0, out.image))
            return false;
        const uint32_t slices = std::max(out.image.layers, 1u);
        out.slice = slice;
        for (const bool words : {true, false})
            out.plane[words ? 0 : 1] = gm20b::xrDescriptorSide(out.image.width, out.image.height,
                                                               slice < slices ? slices - slice : 0, words, source);
        return true;
    }
    if (!gm20b::planeBytes(t->_raw, 4, out.words, !source) || out.words.blockLinear)
        return false;
    out.plane[0] = gm20b::xrSurfaceSide(gm20b::planeReadable(out.words), out.words.pitch, true);
    const bool alpha = gm20b::planeOneRows(t->_raw, out.words.pitch, out.alpha);
    out.plane[1] = gm20b::xrSurfaceSide(alpha ? out.alpha.storage.bytes : 0, out.words.pitch, false);
    return true;
}

void move(gm20b::BlitEncoder *be, const XrSide &from, const XrSide &to, bool words, const gm20b::XrBlock &b)
{
    constexpr uint8_t kZero[16] = {};
    const gm20b::XrPiece &p = b.piece;
    const uint32_t unit = words ? 4 : 1, n = p.bytes / unit;
    if (!p.to.mapped || !n)
        return;
    const gm20b::Texture &fromPlane = words ? from.words : from.alpha, &toPlane = words ? to.words : to.alpha;
    if (!p.from.mapped) {
        if (!to.surface) {
            // the texel's word, or its alpha word, alone (fillRect's words: bit k the texel's word k)
            be->fillRect(to.image, p.to.x, p.to.y, to.slice + p.to.z, n, b.rows, kZero, 8, words ? 1 : 2);
            return;
        }
        gm20b::Texture rows = toPlane;
        rows.va += p.to.at;
        rows.width = rows.texelWidth = n;
        rows.height = rows.texelHeight = b.rows;
        be->fillRect(rows, 0, 0, 0, n, b.rows, kZero, unit, 0);
        return;
    }
    if (!from.surface && !to.surface) {
        be->copyTexels(from.image, p.from.x, p.from.y, from.slice + p.from.z, to.image, p.to.x, p.to.y,
                       to.slice + p.to.z, n, b.rows, words ? gm20b::kAspectDepth : gm20b::kAspectStencil);
        return;
    }
    const bool into = !to.surface;
    const XrSide &image = into ? to : from;
    const gm20b::XrPlace &texel = into ? p.to : p.from, &bytes = into ? p.from : p.to;
    const gm20b::Texture &plane = into ? fromPlane : toPlane;
    be->copyBufferTexels(plane.va + bytes.at, plane.pitch, image.image, texel.x, texel.y, image.slice + texel.z, n,
                         b.rows, into, words ? gm20b::kAspectWhole : gm20b::kAspectStencil);
}

}  // namespace

bool xrDescriptorCopy(SwitchMetalTexture *a, SwitchMetalTexture *b)
{
    return (descriptorXr(a) && (descriptorXr(b) || surfaceXr(b))) || (surfaceXr(a) && descriptorXr(b));
}

bool xrCopyTemps(SwitchMetalTexture *a, SwitchMetalTexture *b)
{
    if (!surfaceXr(a) && !surfaceXr(b))
        return true;
    SwitchMetalTexture *image = descriptorXr(a) ? a : b, *owner = image->_levelsOwner ?: image;
    @synchronized(owner) {
        return gRM->raw->stencilTemp(owner->_raw);
    }
}

void copyXrPlanes(gm20b::BlitEncoder *be, SwitchMetalTexture *s, uint32_t ss, uint32_t sx, uint32_t sy,
                  SwitchMetalTexture *t, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    XrSide from, to;
    if (!xrSide(s, ss, true, from) || !xrSide(t, ds, false, to))
        return;
    for (const bool words : {true, false})
        gm20b::xrCopyBlocks(from.plane[words ? 0 : 1], sx, sy, to.plane[words ? 0 : 1], dx, dy, w, h,
                            [&](const gm20b::XrBlock &b) {
                                move(be, from, to, words, b);
                            });
}
