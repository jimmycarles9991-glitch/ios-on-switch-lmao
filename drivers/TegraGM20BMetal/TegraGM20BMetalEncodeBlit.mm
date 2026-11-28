// SPDX-License-Identifier: MIT
// A blit encoder's commands of the tape onto the copy engine (and the 2D engine's fills and mip chains): see encodeRaw.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalBlitImages.h"
#import "TegraGM20BMetalBlitRules.h"
#import "TegraGM20BMetalCopyClamp.h"
#import "TegraGM20BMetalDecoded.h"
#import "TegraGM20BMetalStandIns.h"
#import "TegraGM20BMetalXrCopies.h"
#include "format_standin.h"
#include "fx/fx_mip_probe.h"
#include "fx/fx_mip_route.h"
#include "attachment_target.h"
#include "blit_overlap.h"
#include "blit_planes.h"
#include "fill_record.h"
#include "mip.h"
#include "overlap_rows.h"
#include "plain_rows.h"
#include "rotated_pass.h"
#include "twod_fill.h"

// What stands for a texture's memory in its blit encoder's order (gm20b::BlitEncoder::access): its surface, its buffer, or the
// texture a view views
static const void *memoryOf(SwitchMetalTexture *t)
{
    if (t && t->_levelsOwner)
        t = t->_levelsOwner;
    if (t && t->_surface)
        return t->_surface;
    if (t && t->_buffer)
        return (__bridge const void *)t->_buffer;
    return (__bridge const void *)t;
}

static const void *memoryOf(SwitchMetalBuffer *b)
{
    return (__bridge const void *)b;
}

// A copy side's z (gm20b::BlitEncoder): an array's or cube's layer, else a 3D texture's plane
static uint32_t layerOrPlane(const gm20b::Texture &t, uint32_t slice, uint32_t z)
{
    return t.layers > 1 ? slice : z;
}

// Single sentinel for kCopyOverlapTag's shared staging memory across all sizes so access() orders reuse.
static const uint8_t kOverlapStageKey = 0;

static bool failNoOverlapStage(const char **why)
{
    *why = "no staging memory for an overlapping texture copy";
    return false;
}

static bool encodeTextureTexels(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey,
                                const void *dstKey, const gm20b::Texture &fromLevel, const gm20b::Texture &toLevel,
                                uint32_t ss, uint32_t ds, uint32_t ex, uint32_t ey, uint32_t sz, uint32_t fx,
                                uint32_t fy, uint32_t dz, uint32_t ew, uint32_t eh, uint32_t planes, uint8_t aspect,
                                bool anyOverlap, const gm20b::ArenaStorage *sArena, const gm20b::ArenaStorage *dArena,
                                const char **why)
{
    const gm20b::VolumeOverlap volOverlap =
        planes > 1 && fromLevel.layers <= 1 && toLevel.layers <= 1
            ? gm20b::volumePlanesOverlap(fromLevel, ex, ey, sz, toLevel, fx, fy, dz, ew, eh, planes, sArena, dArena)
            : gm20b::VolumeOverlap::None;
    if (volOverlap == gm20b::VolumeOverlap::StageAll) {
        uint32_t stagePitch = 0;
        uint64_t planeBytes = 0, totalBytes = 0;
        gm20b::Texture *scratch =
            gm20b::overlapStageLayout(ew, eh, fromLevel.bpp, planes, stagePitch, planeBytes, totalBytes)
                ? rc->copyScratch(totalBytes)
                : nullptr;
        if (!scratch)
            return failNoOverlapStage(why);
        be->access(srcKey, &kOverlapStageKey);
        for (uint32_t k = 0; k < planes; k++) {
            const gm20b::Texture stage =
                gm20b::makeOverlapStage(*scratch, k * planeBytes, ew, eh, fromLevel.bpp, stagePitch);
            if (!be->copyTexels(fromLevel, ex, ey, sz + k, stage, 0, 0, 0, ew, eh, gm20b::kAspectWhole))
                return failNoOverlapStage(why);
        }
        be->access(&kOverlapStageKey, dstKey);
        for (uint32_t k = 0; k < planes; k++) {
            const gm20b::Texture stage =
                gm20b::makeOverlapStage(*scratch, k * planeBytes, ew, eh, fromLevel.bpp, stagePitch);
            if (!be->copyTexels(stage, 0, 0, 0, toLevel, fx, fy, dz + k, ew, eh, aspect))
                return failNoOverlapStage(why);
        }
        return true;
    }
    if (volOverlap == gm20b::VolumeOverlap::Reverse || volOverlap == gm20b::VolumeOverlap::Forward) {
        const bool reverse = volOverlap == gm20b::VolumeOverlap::Reverse;
        be->access(srcKey, dstKey);
        for (uint32_t i = 0; i < planes; i++) {
            if (i)
                be->barrier();
            const uint32_t k = reverse ? planes - 1 - i : i;
            if (!be->copyTexels(fromLevel, ex, ey, sz + k, toLevel, fx, fy, dz + k, ew, eh, aspect))
                return failNoOverlapStage(why);
        }
        return true;
    }
    if (anyOverlap) {
        uint32_t stagePitch = 0;
        uint64_t planeBytes = 0, totalBytes = 0;
        gm20b::Texture *scratch =
            gm20b::overlapStageLayout(ew, eh, fromLevel.bpp, 1, stagePitch, planeBytes, totalBytes)
                ? rc->copyScratch(totalBytes)
                : nullptr;
        if (!scratch)
            return failNoOverlapStage(why);
        const gm20b::Texture stage = gm20b::makeOverlapStage(*scratch, 0, ew, eh, fromLevel.bpp, stagePitch);
        for (uint32_t k = 0; k < planes; k++) {
            const uint32_t from = layerOrPlane(fromLevel, ss, sz + k), to = layerOrPlane(toLevel, ds, dz + k);
            const gm20b::CopyOverlap ov = gm20b::copySideOverlap(gm20b::copySideOf(fromLevel, ex, ey, from),
                                                                 gm20b::copySideOf(toLevel, fx, fy, to), ew, eh,
                                                                 fromLevel.bpp, toLevel.bpp, true, sArena, dArena);
            if (ov == gm20b::CopyOverlap::Identity)
                continue;
            if (ov == gm20b::CopyOverlap::Overlap) {
                be->access(srcKey, &kOverlapStageKey);
                if (!be->copyTexels(fromLevel, ex, ey, from, stage, 0, 0, 0, ew, eh, gm20b::kAspectWhole))
                    return failNoOverlapStage(why);
                be->access(&kOverlapStageKey, dstKey);
                if (!be->copyTexels(stage, 0, 0, 0, toLevel, fx, fy, to, ew, eh, aspect))
                    return failNoOverlapStage(why);
            } else {
                be->access(srcKey, dstKey);
                if (!be->copyTexels(fromLevel, ex, ey, from, toLevel, fx, fy, to, ew, eh, aspect))
                    return failNoOverlapStage(why);
            }
        }
        return true;
    }
    be->access(srcKey, dstKey);
    for (uint32_t k = 0; k < planes; k++) {
        const uint32_t from = layerOrPlane(fromLevel, ss, sz + k), to = layerOrPlane(toLevel, ds, dz + k);
        if (!be->copyTexels(fromLevel, ex, ey, from, toLevel, fx, fy, to, ew, eh, aspect))
            return failNoOverlapStage(why);
    }
    return true;
}

// The copy engine's copies of those rows into or out of plane z: one copy of every row where they lie a pitch apart, else one
// per row
static void copyRows(gm20b::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const gm20b::Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                     uint32_t h, bool intoTexture, uint8_t aspect)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, z, w, h, intoTexture, aspect);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * gm20b::bufferUnit(t, aspect), t, x,
                             y + k, z, w, 1, intoTexture, aspect);
}

// A buffer<->texture copy of `planes` planes from plane z, `image` bytes apart in the buffer (each plane's rows decided at
// record: bufferPlanes); the buffer's lines cleaned before, or invalidated for the CPU after where the copy writes it
void gm20bCopyPlanes(gm20b::BlitEncoder *be, gm20b::CommandBuffer *rc, const gm20b::Buffer &b, uint64_t offset,
                     uint64_t row, uint64_t image, const gm20b::Texture &t, uint32_t x, uint32_t y, uint32_t z,
                     uint32_t w, uint32_t h, uint32_t planes, bool intoTexture, uint8_t aspect)
{
    gm20b::toElements(t, x, y, w, h);  // (whole blocks: checked at record)
    const uint32_t unit = gm20b::bufferUnit(t, aspect), rowBytes = w * unit * std::max<uint32_t>(t.samples, 1);
    for (uint32_t k = 0; k < std::max(planes, 1u); k++) {
        const uint64_t at = offset + k * image;
        BufferRows rows;
        bufferRows(at, row, h, rowBytes, unit, intoTexture, rows);
        const uint64_t first = bufferRow(rows, at, row, 0);
        if (b.hostVisible && !b.wc && b.cpu)
            rc->clean(b.cpu + first, bufferRow(rows, at, row, h - 1) - first + rowBytes, !intoTexture);
        copyRows(be, b.va, rows, at, row, t, x, y, z + k, w, h, intoTexture, aspect);
    }
}

// A no-copy texture copied into a pool image (gm20b::Texture::callerPages): what a copy wrote into the image goes on to the
// caller's memory, which stays the texture's contents, after it on the copy engine. The copy reads the image through its
// kind-0 view after the blit wrote its texels through 0xfe, and later commands use the texels again: a kind switch on
// each side (gpu.astc.layouts blitin_back read the caller's blocks in FermiColor order without them)
static void copyToCallerPages(gm20b::BlitEncoder *be, gm20b::CommandBuffer *rc, SwitchMetalTexture *t)
{
    t = t->_levelsOwner ?: t;
    const gm20b::Texture &x = t->_raw;
    if (!x.callerPages)
        return;
    rc->use(x.callerPages, true);
    rc->clean(x.callerPages->bo.cpu, x.storage.bytes, true);
    be->crossKinds();
    be->copyBytes(x.imagePitchVa, x.callerPages->bo.va, x.storage.bytes);
    be->crossKinds();
}

// The image a buffer copy moves of `t`'s level: a texture held through a surface's planes moves plane 0's bytes, at the
// rows its writes land at `into` it (blit_planes.h; RGB10A8_2P_XR10's colour words: BlitEncoder::copyBufferTexels)
void gm20bBufferCopyImage(SwitchMetalTexture *t, uint32_t level, gm20b::Texture &image, bool into)
{
    blitImage(t, level, image);
    if (t->_raw.converted())
        gm20b::planeBytes(t->_raw, gm20b::planeUnit(t->_raw), image, into);
    else if (into && image.storePitch)
        image = gm20b::storedRows(image, image);
}

static bool texturesStorageOverlap(const gm20b::Texture &a, const gm20b::Texture &b)
{
    const gm20b::Texture::StorageRange ar = a.storageRange(), br = b.storageRange();
    return gm20b::copyRangesOverlap(ar.va, ar.bytes, br.va, br.bytes) ||
           gm20b::arenaCrossKindOverlap(ar.arena, br.arena);
}

// Plane 0's `w` x `h` from (sx, sy) into `to` at (dx, dy, dz): a pitch plane's lines `pitch` apart, each its texels'
// bytes in one run; a block-linear plane's runs in their pieces along the surface's rows (blit_planes.h planeRunPieces)
static bool copyPlaneRuns(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey, const void *dstKey,
                          const gm20b::Texture &from, uint32_t sx, uint32_t sy, const gm20b::Texture &to, uint32_t dx,
                          uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, const char **why)
{
    if (!from.blockLinear) {
        be->access(srcKey, dstKey);
        return be->copyTexels(from, sx, sy, 0, to, dx, dy, dz, w, h) ? true : failNoOverlapStage(why);
    }
    std::vector<gm20b::PlaneRunPiece> pieces;
    gm20b::planeRunPieces(from, sx, sy, w, h, [&](const gm20b::PlaneRunPiece &p) {
        pieces.push_back(p);
    });
    if (pieces.empty())
        return true;
    if (pieces.size() > 1 && texturesStorageOverlap(from, to)) {
        uint32_t stagePitch = 0;
        uint64_t planeBytes = 0, totalBytes = 0;
        gm20b::Texture *scratch = gm20b::overlapStageLayout(w, h, from.bpp, 1, stagePitch, planeBytes, totalBytes)
                                      ? rc->copyScratch(totalBytes)
                                      : nullptr;
        if (!scratch)
            return failNoOverlapStage(why);
        const gm20b::Texture stage = gm20b::makeOverlapStage(*scratch, 0, w, h, from.bpp, stagePitch);
        be->access(srcKey, &kOverlapStageKey);
        for (const gm20b::PlaneRunPiece &p : pieces)
            if (!be->copyTexels(from, p.x, sy + p.row, 0, stage, p.at, 0, 0, p.w, h))
                return failNoOverlapStage(why);
        be->access(&kOverlapStageKey, dstKey);
        return be->copyTexels(stage, 0, 0, 0, to, dx, dy, dz, w, h) ? true : failNoOverlapStage(why);
    }
    be->access(srcKey, dstKey);
    for (const gm20b::PlaneRunPiece &p : pieces)
        if (!be->copyTexels(from, p.x, sy + p.row, 0, to, dx + p.at, dy, dz, p.w, h))
            return failNoOverlapStage(why);
    return true;
}

// Plane 0's bytes of the side or sides held through planes, counted in the destination's texel (kCopyPlaneBytes): the
// copy engine's lines `pitch` apart, each its texels' bytes in one run, which past a row's end are the next rows' (a
// source's lines may overlap); a plain source of another texel size read in it (blit_planes.h imageInUnits)
static bool copyPlaneBytes(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey, const void *dstKey,
                           SwitchMetalTexture *s, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy, uint32_t sz,
                           SwitchMetalTexture *t, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t dz,
                           uint32_t w, uint32_t h, const char **why)
{
    const bool fromPlanes = gm20b::heldInPlanes(s->_raw), intoPlanes = gm20b::heldInPlanes(t->_raw);
    gm20b::Texture from, to;
    if (intoPlanes)
        gm20b::planeBytes(t->_raw, gm20b::planeUnit(t->_raw), to, true);
    else
        blitImage(t, dl, to);
    gm20b::Texture image;
    const uint32_t toZ = intoPlanes ? 0 : layerOrPlane(to, ds, dz);
    if (fromPlanes) {
        gm20b::planeBytesRead(s->_raw, to.bpp, sx, sy, w, h, from);
        return copyPlaneRuns(rc, be, srcKey, dstKey, from, sx, sy, to, dx, dy, toZ, w, h, why);
    }
    if (blitImage(s, sl, image) && image.bpp != to.bpp)
        gm20b::imageInUnits(image, to.bpp, from);
    else
        from = image;
    be->access(srcKey, dstKey);
    return be->copyTexels(from, sx, sy, layerOrPlane(from, ss, sz), to, dx, dy, toZ, w, h) ? true
                                                                                           : failNoOverlapStage(why);
}

// Two textures held through planes (kCopyPlanes): plane 0's bytes in the destination's unit, and plane 1 a byte a texel
// at each side's plane-0 rows on through its end (blit_planes.h planeOneRows), zeros past the source's last page,
// nothing past the destination's; a source of no plane 1 leaves the destination's as it is
static bool copyPlanePair(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey, const void *dstKey,
                          const gm20b::Texture &s, uint32_t sx, uint32_t sy, const gm20b::Texture &d, uint32_t dx,
                          uint32_t dy, uint32_t w, uint32_t h, const char **why)
{
    const uint32_t unit = gm20b::planeUnit(d);
    gm20b::Texture from, to;
    if (!gm20b::planeBytesRead(s, unit, sx, sy, w, h, from) || !gm20b::planeBytes(d, unit, to, true))
        return true;
    gm20b::Texture fromOne, toOne;
    const bool hasPlaneOne = gm20b::planeOneRows(d, to.pitch, toOne) && gm20b::planeOneRows(s, from.pitch, fromOne);
    const bool overlap = texturesStorageOverlap(from, to) ||
                         (hasPlaneOne && (texturesStorageOverlap(fromOne, toOne) ||
                                          texturesStorageOverlap(fromOne, to) || texturesStorageOverlap(from, toOne)));
    constexpr uint8_t kZero[16] = {};
    if (!overlap) {
        if (!copyPlaneRuns(rc, be, srcKey, dstKey, from, sx, sy, to, dx, dy, 0, w, h, why))
            return false;
        if (!hasPlaneOne)
            return true;
        bool ok = true;
        gm20b::planeOneRuns(fromOne.storage.bytes, fromOne.pitch, sx, sy, toOne.storage.bytes, toOne.pitch, dx, dy, w,
                            h, [&](const gm20b::PlaneOneRun &r) {
                                if (!ok)
                                    return;
                                if (r.copied && !be->copyTexels(fromOne, sx, sy + r.row, 0, toOne, dx, dy + r.row, 0,
                                                                r.copied, r.rows)) {
                                    ok = false;
                                    return;
                                }
                                if (r.zeroed)
                                    be->fillRect(toOne, dx + r.copied, dy + r.row, 0, r.zeroed, r.rows, kZero, 1, 0);
                            });
        return ok ? true : failNoOverlapStage(why);
    }
    std::vector<gm20b::PlaneRunPiece> pieces;
    if (from.blockLinear) {
        gm20b::planeRunPieces(from, sx, sy, w, h, [&](const gm20b::PlaneRunPiece &p) {
            pieces.push_back(p);
        });
    }
    std::vector<gm20b::PlaneOneRun> oneRuns;
    if (hasPlaneOne) {
        gm20b::planeOneRuns(fromOne.storage.bytes, fromOne.pitch, sx, sy, toOne.storage.bytes, toOne.pitch, dx, dy, w,
                            h, [&](const gm20b::PlaneOneRun &r) {
                                oneRuns.push_back(r);
                            });
    }
    uint32_t stage0Pitch = 0, stage1Pitch = 0;
    uint64_t plane0Bytes = 0, total0Bytes = 0, plane1Bytes = 0, total1Bytes = 0;
    if (!gm20b::overlapStageLayout(w, h, from.bpp, 1, stage0Pitch, plane0Bytes, total0Bytes))
        return failNoOverlapStage(why);
    if (hasPlaneOne && !gm20b::overlapStageLayout(w, h, 1, 1, stage1Pitch, plane1Bytes, total1Bytes))
        return failNoOverlapStage(why);
    if (total1Bytes > UINT64_MAX - total0Bytes)
        return failNoOverlapStage(why);
    gm20b::Texture *scratch = rc->copyScratch(total0Bytes + total1Bytes);
    if (!scratch)
        return failNoOverlapStage(why);
    const gm20b::Texture stage0 = gm20b::makeOverlapStage(*scratch, 0, w, h, from.bpp, stage0Pitch);
    const gm20b::Texture stage1 =
        hasPlaneOne ? gm20b::makeOverlapStage(*scratch, total0Bytes, w, h, 1, stage1Pitch) : gm20b::Texture();
    be->access(srcKey, &kOverlapStageKey);
    if (!from.blockLinear) {
        if (!be->copyTexels(from, sx, sy, 0, stage0, 0, 0, 0, w, h))
            return failNoOverlapStage(why);
    } else {
        for (const gm20b::PlaneRunPiece &p : pieces)
            if (!be->copyTexels(from, p.x, sy + p.row, 0, stage0, p.at, 0, 0, p.w, h))
                return failNoOverlapStage(why);
    }
    for (const gm20b::PlaneOneRun &r : oneRuns)
        if (r.copied && !be->copyTexels(fromOne, sx, sy + r.row, 0, stage1, 0, r.row, 0, r.copied, r.rows))
            return failNoOverlapStage(why);
    be->access(&kOverlapStageKey, dstKey);
    if (!from.blockLinear || !pieces.empty())
        if (!be->copyTexels(stage0, 0, 0, 0, to, dx, dy, 0, w, h))
            return failNoOverlapStage(why);
    if (!oneRuns.empty())
        be->barrier();
    for (const gm20b::PlaneOneRun &r : oneRuns) {
        if (r.copied && !be->copyTexels(stage1, 0, r.row, 0, toOne, dx, dy + r.row, 0, r.copied, r.rows))
            return failNoOverlapStage(why);
        if (r.zeroed)
            be->fillRect(toOne, dx + r.copied, dy + r.row, 0, r.zeroed, r.rows, kZero, 1, 0);
    }
    return true;
}

// A fill's units into each image of `image` (fill_record.h): a turned plane's region turned as Metal stores it, a
// multisampled one's every sample, a 3D level's planes; the 2D engine's solid rectangle where rule clear.twod holds for the
// format and memory (twod_fill.h), else the copy engine's constants
static void fillImage(gm20b::BlitEncoder *be, gm20b::Texture image, const gm20b::FillRecord &f)
{
    uint32_t x0 = f.x0, y0 = f.y0, x1 = f.x1, y1 = f.y1;
    if (image.rotation) {
        const gm20b::Rect turned = gm20b::frameOf(image).rect({x0, y0, x1, y1});
        x0 = turned.x0, y0 = turned.y0, x1 = turned.x1, y1 = turned.y1;
    }
    if (image.samples > 1) {
        uint32_t gx, gy;
        gm20b::sampleGrid(image.samples, gx, gy);
        x0 <<= gx, x1 <<= gx, y0 <<= gy, y1 <<= gy;
        image.width <<= gx;
        image.height <<= gy;
        image.samples = 1;
    } else if (gm20b::twodFillHolds(image.format, image.comptags) && !f.words && f.bytes == image.bpp &&
               image.type != gm20b::fx::kTexThreeD && (image.blockLinear || image.renderable) && !image.zeta) {
        gm20b::Texture target = image;
        target.va += uint64_t(f.layer) * target.layerBytes;
        target.layers = 1;
        target.type = gm20b::fx::kTexTwoD;
        target.tic = 0;
        target.renderable = true;
        // a crop of the kernel's tiled surface, as gm20b::sideOf places it
        if (image.import && image.import->kernelTiled()) {
            x0 += image.surfaceX, x1 += image.surfaceX, y0 += image.surfaceY, y1 += image.surfaceY;
            target.width = image.pitch / image.bpp;
            target.height = image.import->layout.height;
        }
        uint32_t word = 0;
        memcpy(&word, f.texel, std::min<uint32_t>(f.bytes, sizeof word));
        be->fillTwod(target, x0, y0, x1, y1, word);
        SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.fillTwod++);
        return;
    }
    for (uint32_t z = f.z0; z < f.z1; z++)
        be->fillRect(image, x0, y0, layerOrPlane(image, f.layer, z), x1 - x0, y1 - y0, f.texel, f.bytes, f.words);
}

bool encodeBlit(SwitchMetalCommandBuffer *cb, const Cmd &c, gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be,
                std::set<SwitchMetalTexture *> &fresh, SwitchMetalStandInFresh &standInFresh, const char **why)
{
    switch (c.op) {
    case OpCopyTexture: {
        SwitchMetalTexture *s = (__bridge SwitchMetalTexture *)c.obj, *t = (__bridge SwitchMetalTexture *)c.obj2;
        const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1), dl = extra(cb, x + 9);
        const uint32_t sx = uint32_t(extra(cb, x + 2)), sy = uint32_t(extra(cb, x + 3)),
                       sz = uint32_t(extra(cb, x + 4)), w = uint32_t(extra(cb, x + 5)), h = uint32_t(extra(cb, x + 6)),
                       d = uint32_t(extra(cb, x + 7)), dx = uint32_t(o), dy = uint32_t(o >> 32),
                       dz = uint32_t(extra(cb, x + 11)), ss = uint32_t(extra(cb, x)), ds = uint32_t(extra(cb, x + 8));
        if (c.a == kCopyXrPlanes) {
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            if (!copyXrPlanes(rc, be, memoryOf(s), memoryOf(t), &kOverlapStageKey, s, ss, sx, sy, t, ds, dx, dy, w, h,
                              why))
                return false;
            break;
        }
        if (gm20bFamilyOwner(t)) {
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            be->access(memoryOf(s), memoryOf(t));
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            const uint32_t from[3] = {sx, sy, sz}, to[3] = {dx, dy, dz}, size[3] = {w, h, d};
            if (!gm20bEncodeFamilyTextureCopy(rc, cb, be, s, ss, uint32_t(sl), from, t, ds, uint32_t(dl), to, size,
                                              why))
                return false;
            break;
        }
        if (c.a == kCopyPlanes) {
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            if (!copyPlanePair(rc, be, memoryOf(s), memoryOf(t), s->_raw, sx, sy, t->_raw, dx, dy, w, h, why))
                return false;
            break;
        }
        if (c.a == kCopyPlaneBytes) {
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            if (!copyPlaneBytes(rc, be, memoryOf(s), memoryOf(t), s, uint32_t(sl), ss, sx, sy, sz, t, uint32_t(dl), ds,
                                dx, dy, dz, w, h, why))
                return false;
            if (!gm20b::heldInPlanes(t->_raw))
                copyToCallerPages(be, rc, t);
            break;
        }
        gm20b::Texture fromLevel, toLevel;
        blitImage(s, uint32_t(sl), fromLevel);
        blitImage(t, uint32_t(dl), toLevel);
        if (toLevel.storePitch)  // rows under 16 bytes, which an M4 Max's blit writes 16 apart (plain_rows.h)
            toLevel = gm20b::storedRows(toLevel, toLevel);
        uint32_t ex = sx, ey = sy, ew = w, eh = h;
        gm20b::toElements(s->_raw, ex, ey, ew, eh);  // (whole blocks, the same blocks both sides: checked at record)
        uint32_t fx = dx / t->_raw.blockW, fy = dy / t->_raw.blockH;
        if (fromLevel.samples > 1) {
            uint32_t gx, gy;
            gm20b::sampleGrid(fromLevel.samples, gx, gy);
            for (gm20b::Texture *grid : {&fromLevel, &toLevel}) {
                grid->width <<= gx;
                grid->height <<= gy;
                grid->samples = 1;
            }
            ex <<= gx;
            ey <<= gy;
            ew <<= gx;
            eh <<= gy;
            fx <<= gx;
            fy <<= gy;
        }
        const uint32_t planes = std::max(d, 1u);
        const bool out = c.a == kCopyStencilOut;
        if (out || c.a == kCopyStencilIn) {
            useTexture(rc, s, false);
            useTexture(rc, t, true);
            be->access(memoryOf(s), memoryOf(t));
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            const gm20b::Texture &zeta = out ? fromLevel : toLevel, &bytes = out ? toLevel : fromLevel;
            for (uint32_t k = 0; k < planes; k++) {
                const uint32_t from = layerOrPlane(fromLevel, ss, sz + k), to = layerOrPlane(toLevel, ds, dz + k);
                be->copyStencilTexels(zeta, out ? ex : fx, out ? ey : fy, out ? from : to, bytes, out ? fx : ex,
                                      out ? fy : ey, out ? to : from, ew, eh, !out);
            }
            copyToCallerPages(be, rc, t);
            break;
        }
        const gm20b::ArenaStorage *sArena = fromLevel.storageRange().arena, *dArena = toLevel.storageRange().arena;
        const bool clamps = gm20bCopyClamps(s, uint32_t(sl), t, uint32_t(dl));
        bool allIdentity = true, anyOverlap = false;
        for (uint32_t k = 0; k < planes; k++) {
            const uint32_t from = layerOrPlane(fromLevel, ss, sz + k), to = layerOrPlane(toLevel, ds, dz + k);
            const gm20b::CopyOverlap ov = gm20b::copySideOverlap(gm20b::copySideOf(fromLevel, ex, ey, from),
                                                                 gm20b::copySideOf(toLevel, fx, fy, to), ew, eh,
                                                                 fromLevel.bpp, toLevel.bpp, true, sArena, dArena);
            allIdentity &= ov == gm20b::CopyOverlap::Identity;
            anyOverlap |= ov == gm20b::CopyOverlap::Overlap;
        }
        if (allIdentity) {
            if (!clamps)
                break;
            useTexture(rc, t, true);
            be->access(memoryOf(t), memoryOf(t));
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            if (!gm20bClampCopiedTexture(rc, be, t, uint32_t(dl), ds, dz, d, dx, dy, w, h, why))
                return false;
            copyToCallerPages(be, rc, t);
            break;
        }
        useTexture(rc, s, false);
        useTexture(rc, t, true);
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        if (!encodeTextureTexels(rc, be, memoryOf(s), memoryOf(t), fromLevel, toLevel, ss, ds, ex, ey, sz, fx, fy, dz,
                                 ew, eh, planes, uint8_t(c.a), anyOverlap, sArena, dArena, why))
            return false;
        if (clamps && !gm20bClampCopiedTexture(rc, be, t, uint32_t(dl), ds, dz, d, dx, dy, w, h, why))
            return false;
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyBuffer: {
        SwitchMetalBuffer *s = (__bridge SwitchMetalBuffer *)c.obj, *t = (__bridge SwitchMetalBuffer *)c.obj2;
        useBuffer(rc, s, false);
        useBuffer(rc, t, true);
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
        if (s->_raw.hostVisible && !s->_raw.wc && s->_raw.cpu)
            rc->clean(s->_raw.cpu + c.c, n);
        if (t->_raw.hostVisible && !t->_raw.wc && t->_raw.cpu)
            rc->clean(t->_raw.cpu + dof, n, true);
        be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
        break;
    }
    case OpFillBuffer: {
        SwitchMetalBuffer *t = (__bridge SwitchMetalBuffer *)c.obj;
        const uint64_t n = extra(cb, c.x);
        useBuffer(rc, t, true);
        be->access(nullptr, memoryOf(t));
        if (t->_raw.hostVisible && !t->_raw.wc && t->_raw.cpu)
            rc->clean(t->_raw.cpu + c.c, n, true);
        be->fillBytes(t->_raw.va + c.c, n, uint8_t(c.a));
        break;
    }
    case OpFillTexture: {
        SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)c.obj;
        useTexture(rc, t, true);
        be->access(nullptr, memoryOf(t));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        uint64_t words[gm20b::kFillRecordWords];
        for (uint32_t k = 0; k < gm20b::kFillRecordWords; k++)
            words[k] = extra(cb, c.x + gm20b::kFillCallWords + k);
        gm20b::Texture image;
        blitImage(t, uint32_t(c.c), image);
        fillImage(be, image, gm20b::unpackFill(words));
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyBufferToTexture: {
        SwitchMetalBuffer *s = (__bridge SwitchMetalBuffer *)c.obj;
        SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)c.obj2;
        useBuffer(rc, s, false);
        useTexture(rc, t, true);
        if (SwitchMetalTexture *family = gm20bFamilyOwner(t); family || gm20bDecoded(t)) {
            const uint64_t x = c.x, o = extra(cb, x + 8);
            const uint32_t size[3] = {uint32_t(extra(cb, x + 3)), uint32_t(extra(cb, x + 4)),
                                      uint32_t(extra(cb, x + 5))};
            const uint32_t origin[3] = {uint32_t(o), uint32_t(o >> 32), uint32_t(extra(cb, x + 9))};
            if (!fresh.empty())
                fresh.erase(t);
            standInFresh.erase(t);
            be->access(memoryOf(s), memoryOf(t));
            bool decoded;
            if (family) {
                decoded = gm20bEncodeFamilyUpload(rc, cb, be, s, extra(cb, x), extra(cb, x + 1), extra(cb, x + 2), t,
                                                  size, uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 7)), origin,
                                                  s == family->_keptBlocks, why);
            } else {
                be->end();
                decoded = gm20bEncodeDecodedCopy(rc, s, t, extra(cb, x), extra(cb, x + 1), extra(cb, x + 2), size,
                                                 uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 7)), origin, why);
                be = rc->blit();
            }
            if (!decoded)
                return false;
            break;
        }
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1),
                       image = extra(cb, x + 2);
        const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4), d = extra(cb, x + 5), dl = extra(cb, x + 7),
                       dz = layerOrPlane(t->_raw, uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        if (!dl && gm20b::copiedThroughPool(t->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(w), uint32_t(h))) {
            gm20bCopyPlanes(be, rc, s->_raw, so, row, image, *t->_raw.sampled, 0, 0, 0, uint32_t(w), uint32_t(h), 1,
                            true, uint8_t(c.a));
            be->end();
            const int stored = gm20b::storeCopiedPlainRows(*gRM->raw, *rc, t->_raw, why);
            be = rc->blit();
            if (stored)
                return false;
            break;
        }
        gm20b::Texture level;
        gm20bBufferCopyImage(t, uint32_t(dl), level, true);
        gm20bCopyPlanes(be, rc, s->_raw, so, row, image, level, uint32_t(o), uint32_t(o >> 32), uint32_t(dz),
                        uint32_t(w), uint32_t(h), uint32_t(d), true, uint8_t(c.a));
        if (gm20bCopyClamps(nil, 0, t, uint32_t(dl)) &&
            !gm20bClampCopiedTexture(rc, be, t, uint32_t(dl), uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)),
                                     uint32_t(d), uint32_t(o), uint32_t(o >> 32), uint32_t(w), uint32_t(h), why))
            return false;
        copyToCallerPages(be, rc, t);
        break;
    }
    case OpCopyTextureToBuffer: {
        SwitchMetalTexture *s = (__bridge SwitchMetalTexture *)c.obj;
        SwitchMetalBuffer *t = (__bridge SwitchMetalBuffer *)c.obj2;
        useTexture(rc, s, false);
        useBuffer(rc, t, true);
        if (gm20bFamilyOwner(s)) {
            be->access(memoryOf(s), memoryOf(t));
            const uint64_t x = c.x, o = extra(cb, x + 2);
            const uint32_t size[3] = {uint32_t(extra(cb, x + 4)), uint32_t(extra(cb, x + 5)),
                                      uint32_t(extra(cb, x + 6))};
            const uint32_t origin[3] = {uint32_t(o), uint32_t(o >> 32), uint32_t(extra(cb, x + 3))};
            gm20bEncodeFamilyReadback(rc, be, s, t, extra(cb, x + 7), extra(cb, x + 8), extra(cb, x + 9), size,
                                      uint32_t(extra(cb, x)), uint32_t(extra(cb, x + 1)), origin);
            break;
        }
        be->access(memoryOf(s), memoryOf(t));
        const uint64_t x = c.x, o = extra(cb, x + 2),
                       sz = layerOrPlane(s->_raw, uint32_t(extra(cb, x)), uint32_t(extra(cb, x + 3))),
                       w = extra(cb, x + 4), h = extra(cb, x + 5), d = extra(cb, x + 6), dof = extra(cb, x + 7),
                       row = extra(cb, x + 8), image = extra(cb, x + 9), sl = extra(cb, x + 1);
        gm20b::Texture level;
        gm20bBufferCopyImage(s, uint32_t(sl), level, false);
        gm20bCopyPlanes(be, rc, t->_raw, dof, row, image, level, uint32_t(o), uint32_t(o >> 32), uint32_t(sz),
                        uint32_t(w), uint32_t(h), uint32_t(d), false, uint8_t(c.a));
        if (gm20bCopyClamps(s, uint32_t(sl), nil, 0) &&
            !gm20bClampCopiedBuffer(rc, be, t, dof, row, image, uint32_t(w), uint32_t(h), uint32_t(d), why))
            return false;
        break;
    }
    default:
        break;
    }
    return true;
}

bool gm20bFormatMips(uint32_t format)
{
    const char *state = gm20b::formatMipState(format);
    return state && gRM->raw->ownPipeline(state);
}

// Levels first + 1 .. last of a view's layers by the pack's mip state (mip.h, mip_passes.cpp): a format whose
// words the GM20B cannot filter by its word mip state, one the GM20B renders none of by its own (format_standin.h)
bool mipPasses(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, uint32_t first, uint32_t last, const char **why)
{
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    const uint32_t format = t->_raw.format, firstLayer = t->_slicesBase, layers = t->_raw.layers;
    if (gm20b::wordMipState(format)) {
        useTexture(rc, t, true);
        return gm20b::wordMipPasses(*gRM->raw, *rc, owner->_raw, first, last, firstLayer, layers, format, why);
    }
    if (gm20b::formatStandIn(t->_raw.format)) {
        useTexture(rc, t, true);
        return gm20b::formatMipPasses(*gRM->raw, *rc, owner->_raw, first, last, firstLayer, layers, format, why);
    }
    gm20b::Pipeline *mip = gRM->raw->mipPipeline(t->_raw.format);
    if (!mip)
        return false;
    useTexture(rc, t, true);
    if (!rc->mipPasses(owner->_raw, *mip, first, last, firstLayer, layers, format, why))
        return false;
    SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.mip3dLevels += (last - first) * layers);
    return true;
}

// Levels first + 1 .. last of a 3D texture's chain by the pack's volume mip state (mip.h mip3dState): one pass a plane of
// each level, drawn from a volume header of the level above, its step the two levels' sizes, depths and the plane; a format
// with no such state by its word mip state
static bool mipVolumePasses(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, uint32_t first, uint32_t last,
                            const char **why)
{
    gm20b::Device &d = *gRM->raw;
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    gm20b::Pipeline *mip = d.mip3dPipeline(t->_raw.format);
    const gm20b::fx::MipChain &c = owner->_raw.chain->layout;
    if (!mip && gm20b::wordMip3dState(t->_raw.format)) {
        useTexture(rc, t, true);
        return gm20b::wordMipPasses(d, *rc, owner->_raw, first, last, 0, 1, t->_raw.format, why);
    }
    if (!mip || last >= c.levels)
        return false;
    const gm20b::FormatInfo *f = gm20b::formatInfo(t->_raw.format);
    useTexture(rc, t, true);
    for (uint32_t l = first; l < last; l++) {
        gm20b::Texture from = owner->_raw;
        if (!(from.tic = d.levelTic(owner->_raw, l, f && f->srgb)))
            return false;
        if (from.format != t->_raw.format) {
            from.format = t->_raw.format;
            if (!(from.tic = d.viewTic(from, f->tic0)))
                return false;
        }
        const gm20b::fx::MipLevel &above = c.level[l], &level = c.level[l + 1];
        for (uint32_t z = 0; z < level.depth; z++) {
            gm20b::Texture to;
            if (!gm20b::attachmentTarget(owner->_raw, l + 1, z, to))
                return false;
            to.format = t->_raw.format;
            to.renderable = f->rt != 0;
            to.layers = 1;
            gm20b::PassDesc p;
            p.color[0].texture = &to;
            p.color[0].load = gm20b::ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            p.width = level.width;
            p.height = level.height;
            gm20b::RenderEncoder *e = rc->render(p);
            if (!e)
                return false;
            e->setPipeline(mip);
            e->setTexture(0, &from);
            const uint32_t step[8] = {above.width, above.height, level.width, level.height,
                                      above.depth, level.depth,  z,           0};
            e->setBytes(2, 0, step, sizeof step);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
            e->end();
        }
    }
    SWITCH_METAL_DIAGNOSTIC(d.stats.mip3dLevels += last - first);
    return true;
}

// generateMipmapsForTexture: each run of the chain's steps on its engine (fx_mip_route.h, decided at record), the blit
// encoder ended around 3D runs and `be` the one encoding goes on with. `probe2d`: a debug build's probe of a format the pack's
// mip state also makes, every step on the 2D engine as that surface format (fx_mip_probe.h)
bool encodeMipmaps(gm20b::CommandBuffer *rc, SwitchMetalTexture *t, uint32_t probe2d, gm20b::BlitEncoder *&be,
                   const char **why)
{
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    const gm20b::Texture::Chain &chain = *owner->_raw.chain;
    const uint32_t first = t->_levelsBase, last = first + uint32_t(t->_descriptor.mipmapLevelCount) - 1;
    if (t->_raw.type == gm20b::fx::kTexThreeD) {
        be->end();
        const bool ok = mipVolumePasses(rc, t, first, last, why);
        be = rc->blit();
        return ok;
    }
    if (gm20b::fx::integerFormat(t->_raw.format)) {
        useTexture(rc, t, true);
        be->access(memoryOf(t), memoryOf(t));
        be->generate(owner->_raw, gm20b::fx::twodPointFormat(t->_raw.bpp), first, last, 0, t->_slicesBase,
                     t->_raw.layers, true);
        return true;
    }
    const uint32_t fmt2d = probe2d                                ? probe2d
                           : t->_raw.format == owner->_raw.format ? chain.fmt2d
                                                                  : gm20b::fx::twodChainFormat(t->_raw.format);
    gm20b::Device &d = *gRM->raw;
    bool ok = true;
    const bool routed = gm20b::fx::eachMipRun(
        chain.layout, first, last, fmt2d != 0,
        !probe2d && (d.mipPipeline(t->_raw.format) || gm20bFormatMips(t->_raw.format) ||
                     d.ownPipeline(gm20b::wordMipState(t->_raw.format))),
        [&](const gm20b::fx::MipRun &run) {
            if (!ok)
                return;
            if (run.engine == gm20b::fx::MipEngine::TwoD) {
                useTexture(rc, t, true);
                be->access(memoryOf(t), memoryOf(t));
                be->generate(owner->_raw, fmt2d, run.first, run.last, run.unproven, t->_slicesBase, t->_raw.layers);
                return;
            }
            be->end();
            ok = mipPasses(rc, t, run.first, run.last, why);
            be = rc->blit();
        });
    return routed && ok;
}
