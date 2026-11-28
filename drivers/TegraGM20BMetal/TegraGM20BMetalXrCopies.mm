// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalXrCopies.h"
#import "TegraGM20BMetalBlitImages.h"
#include "blit_overlap.h"
#include "blit_planes.h"
#include "xr.h"
#include "xr_copy.h"

#include <array>
#include <vector>

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

bool move(gm20b::BlitEncoder *be, const XrSide &from, const XrSide &to, bool words, const gm20b::XrBlock &b)
{
    constexpr uint8_t kZero[16] = {};
    const gm20b::XrPiece &p = b.piece;
    const uint32_t unit = words ? 4 : 1, n = p.bytes / unit;
    if (!p.to.mapped || !n)
        return true;
    const gm20b::Texture &fromPlane = words ? from.words : from.alpha, &toPlane = words ? to.words : to.alpha;
    if (!p.from.mapped) {
        if (!to.surface) {
            // the texel's word, or its alpha word, alone (fillRect's words: bit k the texel's word k)
            be->fillRect(to.image, p.to.x, p.to.y, to.slice + p.to.z, n, b.rows, kZero, 8, words ? 1 : 2);
            return true;
        }
        gm20b::Texture rows = toPlane;
        rows.va += p.to.at;
        rows.width = rows.texelWidth = n;
        rows.height = rows.texelHeight = b.rows;
        be->fillRect(rows, 0, 0, 0, n, b.rows, kZero, unit, 0);
        return true;
    }
    if (!from.surface && !to.surface) {
        return be->copyTexels(from.image, p.from.x, p.from.y, from.slice + p.from.z, to.image, p.to.x, p.to.y,
                              to.slice + p.to.z, n, b.rows, words ? gm20b::kAspectDepth : gm20b::kAspectStencil);
    }
    const bool into = !to.surface;
    const XrSide &image = into ? to : from;
    const gm20b::XrPlace &texel = into ? p.to : p.from, &bytes = into ? p.from : p.to;
    const gm20b::Texture &plane = into ? fromPlane : toPlane;
    be->copyBufferTexels(plane.va + bytes.at, plane.pitch, image.image, texel.x, texel.y, image.slice + texel.z, n,
                         b.rows, into, words ? gm20b::kAspectWhole : gm20b::kAspectStencil);
    return true;
}

bool xrDescriptorsOverlap(const gm20b::Texture &a, const gm20b::Texture &b)
{
    const gm20b::Texture::StorageRange ar = a.storageRange(), br = b.storageRange();
    return gm20b::copyRangesOverlap(ar.va, ar.bytes, br.va, br.bytes) ||
           gm20b::arenaCrossKindOverlap(ar.arena, br.arena);
}

bool failNoXrStage(const char **why)
{
    *why = "no staging memory for an overlapping texture copy";
    return false;
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
    if (__atomic_load_n(&owner->_raw.stencilTemp, __ATOMIC_ACQUIRE))
        return true;
    os_unfair_lock_lock(&owner->_auxLock);
    const bool made = gRM->raw->stencilTemp(owner->_raw);
    os_unfair_lock_unlock(&owner->_auxLock);
    return made;
}

bool copyXrPlanes(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey, const void *dstKey,
                  const void *stageKey, SwitchMetalTexture *s, uint32_t ss, uint32_t sx, uint32_t sy,
                  SwitchMetalTexture *t, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                  const char **why)
{
    XrSide from, to;
    if (!xrSide(s, ss, true, from) || !xrSide(t, ds, false, to))
        return true;
    if (!from.surface && !to.surface && xrDescriptorsOverlap(from.image, to.image)) {
        struct StagedXrBlock {
            bool words;
            gm20b::XrBlock block;
            uint64_t offset;
            uint32_t pitch;
            uint32_t n;
            bool staged;
        };

        std::vector<StagedXrBlock> blocks;
        uint64_t totalBytes = 0;
        for (const bool words : {true, false}) {
            const uint32_t unit = words ? 4 : 1;
            gm20b::xrCopyBlocks(from.plane[words ? 0 : 1], sx, sy, to.plane[words ? 0 : 1], dx, dy, w, h,
                                [&](const gm20b::XrBlock &b) {
                                    const uint32_t n = b.piece.bytes / unit;
                                    if (!b.piece.to.mapped || !n)
                                        return;
                                    blocks.push_back({words, b, 0, 0, n, b.piece.from.mapped});
                                });
        }
        for (StagedXrBlock &sb : blocks) {
            if (!sb.staged)
                continue;
            uint32_t pitch = 0;
            uint64_t planeBytes = 0, blockBytes = 0;
            if (!gm20b::overlapStageLayout(sb.n, sb.block.rows, from.image.bpp, 1, pitch, planeBytes, blockBytes) ||
                blockBytes > UINT64_MAX - totalBytes)
                return failNoXrStage(why);
            sb.offset = totalBytes;
            sb.pitch = pitch;
            totalBytes += blockBytes;
        }
        gm20b::Texture *scratch = totalBytes ? rc->copyScratch(totalBytes) : nullptr;
        if (totalBytes && !scratch)
            return failNoXrStage(why);
        if (totalBytes) {
            be->access(srcKey, stageKey);
            for (const StagedXrBlock &sb : blocks) {
                if (!sb.staged)
                    continue;
                const gm20b::Texture stage =
                    gm20b::makeOverlapStage(*scratch, sb.offset, sb.n, sb.block.rows, from.image.bpp, sb.pitch);
                const gm20b::XrPiece &p = sb.block.piece;
                if (!be->copyTexels(from.image, p.from.x, p.from.y, from.slice + p.from.z, stage, 0, 0, 0, sb.n,
                                    sb.block.rows, gm20b::kAspectWhole))
                    return failNoXrStage(why);
            }
            be->access(stageKey, dstKey);
        } else {
            be->access(srcKey, dstKey);
        }
        constexpr uint8_t kZero[16] = {};
        for (const StagedXrBlock &sb : blocks) {
            const gm20b::XrPiece &p = sb.block.piece;
            if (!sb.staged) {
                be->fillRect(to.image, p.to.x, p.to.y, to.slice + p.to.z, sb.n, sb.block.rows, kZero, 8,
                             sb.words ? 1 : 2);
                continue;
            }
            const gm20b::Texture stage =
                gm20b::makeOverlapStage(*scratch, sb.offset, sb.n, sb.block.rows, from.image.bpp, sb.pitch);
            if (!be->copyTexels(stage, 0, 0, 0, to.image, p.to.x, p.to.y, to.slice + p.to.z, sb.n, sb.block.rows,
                                sb.words ? gm20b::kAspectDepth : gm20b::kAspectStencil))
                return failNoXrStage(why);
        }
        return true;
    }
    be->access(srcKey, dstKey);
    bool ok = true;
    for (const bool words : {true, false}) {
        gm20b::xrCopyBlocks(from.plane[words ? 0 : 1], sx, sy, to.plane[words ? 0 : 1], dx, dy, w, h,
                            [&](const gm20b::XrBlock &b) {
                                if (ok && !move(be, from, to, words, b))
                                    ok = false;
                            });
        if (!ok)
            return failNoXrStage(why);
    }
    return true;
}
