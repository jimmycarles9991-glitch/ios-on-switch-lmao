// SPDX-License-Identifier: MIT
// The blit encoder's copies as recorded: Metal's rules and plain Metal's answers (TegraGM20BMetalBlitRules.h) decide each one,
// which the copy engine then moves (TegraGM20BMetalEncodeBlit.mm). Fills and mipmaps: TegraGM20BMetalBlitFill.mm.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalResolveAsk.h"
#import "TegraGM20BMetalBlitImages.h"
#import "TegraGM20BMetalBlitRules.h"
#import "TegraGM20BMetalDecoded.h"
#import "TegraGM20BMetalXrCopies.h"
#include "blit_planes.h"

namespace {
// How a texture-to-texture copy of neither side held through planes moves its texels (OpCopyTexture's a): its texels, a
// stencil view's stencil into another's (gm20b::kAspectStencil), or a stencil view's stencil out of or into a texture of
// one-byte texels
uint32_t textureCopyForm(SwitchMetalTexture *a, SwitchMetalTexture *b)
{
    const bool fromStencil = a->_descriptor.pixelFormat == MTLPixelFormatX32_Stencil8,
               toStencil = b->_descriptor.pixelFormat == MTLPixelFormatX32_Stencil8;
    if (fromStencil && toStencil)
        return gm20b::kAspectStencil;
    return fromStencil ? kCopyStencilOut : toStencil ? kCopyStencilIn : gm20b::kAspectWhole;
}

// The bytes `image`'s memory holds from its first
uint64_t imageBytes(const gm20b::Texture &image)
{
    const gm20b::Texture::StorageRange r = image.storageRange();
    return r.bytes && image.va >= r.va && image.va - r.va < r.bytes ? r.va + r.bytes - image.va : 0;
}

// A copy one of whose sides is held through planes, as an M4 Max moves it with validation off (blit_planes.h):
// plane 0's bytes counted in the destination's texel (a plain texture's, or plane 0's unit), a plain source of another
// texel size read in that unit, its rows' bytes in its own layout (imageInUnits), and between two such textures plane 1
// too, a byte a texel at each side's plane-0 rows (kCopyPlanes). Each side's region is inside its texture
// (copySideInvalid). Null: the form; else why Metal's validation stops the copy, and of what past it the driver moves
// nothing: a multisampled, depth or block-compressed plain side, a region that reads past its surface's last page or
// its texture's memory, or one past a plane's rows
const char *planesCopyForm(SwitchMetalTexture *a, NSUInteger sl, MTLOrigin so, MTLSize size, SwitchMetalTexture *b,
                           MTLOrigin dorg, uint32_t &form)
{
    const gm20b::Texture &s = a->_raw, &d = b->_raw;
    const bool fromPlanes = gm20b::heldInPlanes(s), intoPlanes = gm20b::heldInPlanes(d);
    const gm20b::Texture &plain = fromPlanes ? d : s;
    if (!(fromPlanes && intoPlanes) && (plain.samples > 1 || plain.zeta || plain.blockW > 1 || plain.blockH > 1))
        return "between a texture held through planes and a multisampled, depth or block-compressed one";
    const uint32_t unit = intoPlanes ? gm20b::planeUnit(d) : d.bpp;
    gm20b::Texture from, to, image;
    if (!fromPlanes && s.bpp != unit && a->_descriptor.pixelFormat == b->_descriptor.pixelFormat)
        return "into a texture held through planes from one of another texel size than its plane 0's";
    if (!fromPlanes && s.bpp != unit &&
        (!blitImage(a, uint32_t(sl), image) || !gm20b::imageInUnits(image, unit, from) ||
         !gm20b::unitRegionReadable(from, imageBytes(image), so.x, so.y, size.width, size.height)))
        return "into a texture held through planes, read in its unit past its source's memory";
    if (fromPlanes &&
        (!gm20b::planeBytes(s, unit, from) || !gm20b::planeRegionReadable(from, so.x, so.y, size.width, size.height)))
        return "of a region that reads past the end of its surface's last page";
    if (intoPlanes &&
        (!gm20b::planeBytes(d, unit, to, true) || dorg.x + size.width > to.width || dorg.y + size.height > to.height))
        return "of a region past a plane's rows";
    form = fromPlanes && intoPlanes ? kCopyPlanes : kCopyPlaneBytes;
    return nullptr;
}

// Whether a copy between texture `d` and buffer `s` is the driver's (`aspect` the aspect it moves, `o` and `size`
// placed in the level where it writes the texture); false: the copy was skipped or its command buffer failed
bool bufferCopy(__unsafe_unretained SwitchMetalCommandBuffer *cb, const char *call, id<MTLBuffer> s, id<MTLTexture> d,
                NSUInteger slice, NSUInteger level, MTLOrigin &o, MTLSize &size, NSUInteger offset, NSUInteger row,
                NSUInteger image, MTLBlitOption options, bool intoTexture, int &aspect)
{
    SwitchMetalBuffer *a = [(id)s isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)s : nil;
    aspect = gm20b::kAspectWhole;
    if (!a) {
        gm20bCallInvalid(cb, PredCallBufForeign, offset, 0,
                         s ? "a blit of a buffer not made by this device" : "a blit of a nil buffer");
        return false;
    }
    if (!d) {
        gm20bCallInvalid(cb, PredTexForeign, 0, 0, "a blit of a nil texture");
        return false;
    }
    SwitchMetalTexture *t = gm20bTexture(d);
    if (!t) {
        notRawTexture(cb, d);
        return false;
    }
    if (gm20bDecoded(t)) {
        if (!intoTexture && !gm20bFamilyOwner(t)) {
            gm20bDecodedReadback(t, call);
            return false;
        }
        return gm20bDecodedCopyRecorded(cb, call, a, t, slice, level, o, size, offset, row, image);
    }
    gm20b::Texture plane;
    if (t->_raw.converted() && !gm20b::planeBytes(t->_raw, gm20b::planeUnit(t->_raw), plane)) {
        failNoImage(cb, call);
        return false;
    }
    if ((aspect = blitAspect(t, options)) == kAspectNoMemory) {
        failNoStencilTemp(cb, call);
        return false;
    }
    if (intoTexture) {
        roundToBlocks(call, t, o);
        cutToLevel(call, t, level, o, size);
    }
    if (const char *invalid =
            bufferCopyInvalid(t, slice, level, o, size, offset, row, image, a->_raw.length, aspect, intoTexture)) {
        invalidBlit(cb, call, invalid);
        return false;
    }
    if (!copySideOk(t, slice, level, o, size)) {
        failNoImage(cb, call);
        return false;
    }
    if (t->_descriptor.sampleCount > 1) {
        SwitchMetalTexture *owner = t->_levelsOwner ?: t;
        bool made;
        @synchronized(owner) {
            made = gRM->raw->sampleRows(owner->_raw);
        }
        if (!made) {
            failNoStencilTemp(cb, call);
            return false;
        }
    }
    return true;
}
}  // namespace

@implementation SwitchMetalBlitEncoder

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    constexpr const char *call = "copyFromTexture:toTexture:";
    if (!s || !d)
        return gm20bCallInvalid(_cb, PredTexForeign, 0, 0, "a blit of a nil texture");
    SwitchMetalTexture *a = gm20bTexture(s), *b = gm20bTexture(d);
    uint32_t form = gm20b::kAspectWhole;
    if (!a || !b)
        return notRawTexture(_cb, a ? d : s);
    flatSlice(call, a, ss);
    if (xrDescriptorCopy(a, b)) {
        if (const char *invalid = copySideInvalid(a, ss, sl, so, size) ?: copySideInvalid(b, ds, dl, dorg, size))
            return invalidBlit(_cb, call, invalid);
        if (a->_descriptor.pixelFormat != b->_descriptor.pixelFormat)
            copyAcrossFormats(call);
        if (!xrCopyTemps(a, b))
            return failNoStencilTemp(_cb, call);
        form = kCopyXrPlanes;
    } else if (gm20b::heldInPlanes(a->_raw) || gm20b::heldInPlanes(b->_raw)) {
        if (const char *invalid = copySideInvalid(a, ss, sl, so, size) ?: copySideInvalid(b, ds, dl, dorg, size)
                                                                     ?: planesCopyForm(a, sl, so, size, b, dorg, form))
            return invalidBlit(_cb, call, invalid);
        if (a->_descriptor.pixelFormat != b->_descriptor.pixelFormat)
            copyAcrossFormats(call);
        if ((!gm20b::heldInPlanes(a->_raw) && !copySideOk(a, ss, sl, so, size)) ||
            (!gm20b::heldInPlanes(b->_raw) && !copySideOk(b, ds, dl, dorg, size)))
            return failNoImage(_cb, call);
    } else {
        cutToLevel(call, a, sl, so, size);
        if (const char *invalid = textureCopyInvalid(a, ss, sl, so, size, b, ds, dl, dorg))
            return invalidBlit(_cb, call, invalid);
        form = textureCopyForm(a, b);
        if ((form == kCopyStencilOut || form == kCopyStencilIn) &&
            blitAspect(form == kCopyStencilOut ? a : b, MTLBlitOptionNone) == kAspectNoMemory)
            return failNoStencilTemp(_cb, call);
        if (!copySideOk(a, ss, sl, so, size) || !copySideOk(b, ds, dl, dorg, size))
            return failNoImage(_cb, call);
    }
    [_cb retain:s];
    [_cb retain:d];
    noteReadAsData(s);
    if ([(id)d isKindOfClass:[SwitchMetalTexture class]])
        declareGpuWrites((SwitchMetalTexture *)d);
    Cmd m = cmd(OpCopyTexture);
    m.a = form;
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[11] = {ss,
                            sl,
                            so.x,
                            so.y,
                            so.z,
                            size.width,
                            size.height,
                            size.depth,
                            ds,
                            dl,
                            uint64_t(dorg.x) | uint64_t(dorg.y) << 32};
    _cb->_extra.append(e, e + 11);
    _cb->_extra.push_back(dorg.z);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
             sliceCount:(NSUInteger)slices
             levelCount:(NSUInteger)levels
{
    for (NSUInteger k = 0; k < slices; k++)
        for (NSUInteger l = 0; l < levels; l++)
            [self copyFromTexture:s
                      sourceSlice:ss + k
                      sourceLevel:sl + l
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:levelSize(s, sl + l)
                        toTexture:d
                 destinationSlice:ds + k
                 destinationLevel:dl + l
                destinationOrigin:MTLOriginMake(0, 0, 0)];
}

// Metal's whole-texture copy (MTLBlitCommandEncoder.h): every level of a size both have, from the larger's level of the
// smaller's size, and as many slices as both have
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    NSUInteger sl = 0, dl = 0;
    if (!sameSizeLevels(s, d, sl, dl))
        return gm20bCallInvalid(
            _cb, PredCallBlit, 0, 0,
            "copyFromTexture:toTexture: between textures neither of which has a level of the other's "
            "first level's size");
    [self copyFromTexture:s
              sourceSlice:0
              sourceLevel:sl
                toTexture:d
         destinationSlice:0
         destinationLevel:dl
               sliceCount:std::min(sliceCount(s), sliceCount(d))
               levelCount:std::min(s.mipmapLevelCount - sl, d.mipmapLevelCount - dl)];
}

- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)value
{
    SwitchMetalBuffer *t = (SwitchMetalBuffer *)b;
    if (![(id)t isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bCallInvalid(_cb, PredCallBufForeign, NSMaxRange(r), 0,
                                b ? "a fill of a buffer not made by this device" : "a fill of a nil buffer");
    if (NSMaxRange(r) > t->_raw.length) {
        skip(_cb, PredCallBufCopyRange, NSMaxRange(r), t->_raw.length, "a fill past its buffer, clamped to it");
        r.length = NSUInteger(gm20b::clampedCopy(t->_raw.length, r.location, t->_raw.length, r.location, r.length));
        if (!r.length)
            return;
    }
    [_cb retain:b];
    Cmd m = cmd(OpFillBuffer);
    m.obj = (__bridge void *)b;
    m.a = value;
    m.c = r.location;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(r.length);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dof
                  size:(NSUInteger)n
{
    SwitchMetalBuffer *a = (SwitchMetalBuffer *)s, *b = (SwitchMetalBuffer *)d;
    if (![(id)a isKindOfClass:[SwitchMetalBuffer class]] || ![(id)b isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bCallInvalid(_cb, PredCallBufForeign, so + n, dof + n,
                                s && d ? "a copy of a buffer not made by this device" : "a copy of a nil buffer");
    if (so + n > a->_raw.length || dof + n > b->_raw.length) {
        skip(_cb, PredCallBufCopyRange, so + n, dof + n, "a copy past its buffers, clamped to both");
        n = NSUInteger(gm20b::clampedCopy(a->_raw.length, so, b->_raw.length, dof, n));
        if (!n)
            return;
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = 0;
    m.c = so;
    m.x = _cb->_extra.size();
    _cb->_extra.push_back(dof);
    _cb->_extra.push_back(n);
    [_cb record:m];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
{
    [self copyFromBuffer:s
               sourceOffset:so
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:d
           destinationSlice:ds
           destinationLevel:dl
          destinationOrigin:dorg
                    options:MTLBlitOptionNone];
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)so
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dorg
                options:(MTLBlitOption)options
{
    int aspect;
    if (!bufferCopy(_cb, "copyFromBuffer:toTexture:", s, d, ds, dl, dorg, size, so, row, image, options, true, aspect))
        return;
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[SwitchMetalTexture class]])
        declareGpuWrites((SwitchMetalTexture *)d);
    Cmd m = cmd(OpCopyBufferToTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = uint32_t(aspect);
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        so, row, image, size.width, size.height, size.depth, ds, dl, uint64_t(dorg.x) | uint64_t(dorg.y) << 32, dorg.z};
    _cb->_extra.append(e, e + 10);
    [_cb record:m];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    [self copyFromTexture:s
                     sourceSlice:ss
                     sourceLevel:sl
                    sourceOrigin:so
                      sourceSize:size
                        toBuffer:d
               destinationOffset:dof
          destinationBytesPerRow:row
        destinationBytesPerImage:image
                         options:MTLBlitOptionNone];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)ss
                 sourceLevel:(NSUInteger)sl
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)size
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)dof
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
                     options:(MTLBlitOption)options
{
    int aspect;
    if (!bufferCopy(_cb, "copyFromTexture:toBuffer:", d, s, ss, sl, so, size, dof, row, image, options, false, aspect))
        return;
    [_cb retain:s];
    [_cb retain:d];
    noteReadAsData(s);
    Cmd m = cmd(OpCopyTextureToBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.a = uint32_t(aspect);
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        ss, sl, uint64_t(so.x) | uint64_t(so.y) << 32, so.z, size.width, size.height, size.depth, dof, row, image};
    _cb->_extra.append(e, e + 10);
    [_cb record:m];
}

- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;  // shared memory: nothing to make visible
}

- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l
{
    (void)t;
    (void)s;
    (void)l;
}

- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t
{
    (void)t;  // pitch-linear memory either way
}

- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t
{
    (void)t;
}

@end
