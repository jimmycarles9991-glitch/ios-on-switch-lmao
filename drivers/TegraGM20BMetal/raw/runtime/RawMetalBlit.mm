// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp, bool intoTexture,
                BufferRows &out)
{
    const uint32_t word = std::min<uint32_t>(bpp, 4);
    if (offset % bpp == 0 && bytesPerRow % bpp == 0 && (rows < 2 || bytesPerRow >= rowBytes)) {  // Metal's rules
        out = {offset, bytesPerRow, true, word};
        return true;
    }
    if (rows > 1 && bytesPerRow < rowBytes && !intoTexture)
        return false;
    const uint64_t pitch = bytesPerRow - bytesPerRow % bpp;
    if (intoTexture && rows > 1 && pitch % 16 == 0 && offset % 16 == 0) {
        if (offset || pitch < rowBytes)
            return false;
        out = {0, pitch, true, word};
        return true;
    }
    out = {offset - offset % word, bytesPerRow, offset % word == 0 && bytesPerRow % word == 0, word};
    return true;
}

// Row y of `r` (offset and bytesPerRow as given to bufferRows)
uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y)
{
    if (r.uniform)
        return r.first + y * r.pitch;
    const uint64_t at = offset + uint64_t(y) * bytesPerRow;
    return at - at % r.word;
}

// Blit options choose a depth/stencil aspect or a PVRTC row layout; a colour texture the raw path holds has neither, so they
// change nothing about its copy (m2v's replay of the copy never carried them)
static bool blitOptionsMoot(id<MTLTexture> t, MTLBlitOption options)
{
    if (!options)
        return true;
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    return x && x->_rawOk && !x->_raw.zeta && !x->_raw.converted();
}

// `w` x `h` texels at (x, y) of level `level` of `t` as one side of a raw copy: level 0 the texture's memory, a later level
// its chain's (raw::chainLevelImage)
static bool copySideOk(RMTexture *t, NSUInteger level, NSUInteger x, NSUInteger y, NSUInteger w, NSUInteger h)
{
    if (!t->_rawOk || t->_raw.memoryless)
        return false;
    raw::Texture view;
    if (level && (level >= raw::fx::kMaxLevels || !raw::chainLevelImage(t->_raw, uint32_t(level), view)))
        return false;
    const raw::Texture &side = level ? view : t->_raw;
    return x + w <= side.width && y + h <= side.height;
}

static MTLSize levelSize(id<MTLTexture> t, NSUInteger level)
{
    auto at = [&](NSUInteger n) {
        return std::max<NSUInteger>(1, n >> level);
    };
    return MTLSizeMake(at(t.width), at(t.height), t.textureType == MTLTextureType3D ? at(t.depth) : 1);
}

static NSUInteger sliceCount(id<MTLTexture> t)
{
    const MTLTextureType type = t.textureType;
    return (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray ? 6 : 1) *
           std::max<NSUInteger>(1, t.arrayLength);
}

// The levels a whole-texture copy starts at: the smaller's first, and the larger's of its size. False: neither has a level
// of the other's size.
static bool sameSizeLevels(id<MTLTexture> s, id<MTLTexture> d, NSUInteger &sl, NSUInteger &dl)
{
    auto same = [](MTLSize a, MTLSize b) {
        return a.width == b.width && a.height == b.height && a.depth == b.depth;
    };
    sl = dl = 0;
    for (NSUInteger l = 0; l < s.mipmapLevelCount; l++)
        if (same(levelSize(s, l), levelSize(d, 0))) {
            sl = l;
            return true;
        }
    for (NSUInteger l = 1; l < d.mipmapLevelCount; l++)
        if (same(levelSize(d, l), levelSize(s, 0))) {
            dl = l;
            return true;
        }
    return false;
}

@implementation RMBlitEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBBlitEncoder");
}

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
    RMTexture *a = (RMTexture *)s, *b = (RMTexture *)d;
    const bool both = [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMTexture class]];
    // the copy engine's, of bytes, between pitch and block-linear sides as each side is laid out
    const bool ok = both && !ss && !ds && size.depth == 1 && a->_raw.bpp == b->_raw.bpp &&
                    copySideOk(a, sl, so.x, so.y, size.width, size.height) &&
                    copySideOk(b, dl, dorg.x, dorg.y, size.width, size.height);
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture copy the raw path does not do (levels, slices, formats)"];
        hit(_cb, PredBlitTexCopy, ss | sl << 8 | ds << 16 | dl << 24,
            uint64_t(size.width) << 32 | uint32_t(size.height));
    } else if (a->_raw.converted() || b->_raw.converted()) {
        // Metal's copies move a planar texture's planes' bytes: m2v's
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, a->_raw.converted(), b->_raw.converted());
    }
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyTexture);
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
    _cb->_extra.insert(_cb->_extra.end(), e, e + 11);
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

// Metal's whole-texture copy: every level of a size both have, from the larger's level of the smaller's size, and as many
// slices as both have
- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    NSUInteger sl = 0, dl = 0;
    if (s.textureType == d.textureType && sameSizeLevels(s, d, sl, dl)) {
        [self copyFromTexture:s
                  sourceSlice:0
                  sourceLevel:sl
                    toTexture:d
             destinationSlice:0
             destinationLevel:dl
                   sliceCount:std::min(sliceCount(s), sliceCount(d))
                   levelCount:std::min(s.mipmapLevelCount - sl, d.mipmapLevelCount - dl)];
        return;
    }
    [self copyFromTexture:s
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(s.width, s.height, 1)
                toTexture:d
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (s.mipmapLevelCount > 1 || s.arrayLength > 1 || s.textureType != MTLTextureType2D) {
        [_cb takeReason:WhyTexture why:@"a whole-texture copy of several levels or slices"];
        hit(_cb, PredBlitWholeTexCopy, s.mipmapLevelCount, s.arrayLength);
    }
}

- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)value
{
    RMBuffer *t = (RMBuffer *)b;
    if (![(id)t isKindOfClass:[RMBuffer class]] || NSMaxRange(r) > t->_raw.length) {
        [_cb takeReason:WhyCall why:@"a buffer fill out of range"];
        hit(_cb, PredCallBufCopyRange, NSMaxRange(r), [(id)t isKindOfClass:[RMBuffer class]] ? t->_raw.length : 0);
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
    RMBuffer *a = (RMBuffer *)s, *b = (RMBuffer *)d;
    if (![(id)a isKindOfClass:[RMBuffer class]] || ![(id)b isKindOfClass:[RMBuffer class]] || so + n > a->_raw.length ||
        dof + n > b->_raw.length) {
        [_cb takeReason:WhyCall why:@"a buffer copy out of range"];
        hit(_cb, PredCallBufCopyRange, so + n, dof + n);
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
    RMBuffer *a = (RMBuffer *)s;
    RMTexture *b = (RMTexture *)d;
    BufferRows rows;
    const bool ok =
        [(id)a isKindOfClass:[RMBuffer class]] && [(id)b isKindOfClass:[RMTexture class]] && !ds && size.depth == 1 &&
        copySideOk(b, dl, dorg.x, dorg.y, size.width, size.height) && b->_raw.bpp &&
        bufferRows(so, row, uint32_t(size.height), uint32_t(size.width * b->_raw.bpp), b->_raw.bpp, true, rows) &&
        bufferRow(rows, so, row, uint32_t(size.height - 1)) + size.width * b->_raw.bpp <= a->_raw.length;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a buffer-to-texture copy the raw path does not do"];
        hit(_cb, PredBlitBufToTex, row, uint64_t(ds) | uint64_t(dl) << 8 | uint64_t(size.depth) << 16);
    } else if (b->_raw.converted()) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, 0, 1);
    }
    [_cb retain:s];
    [_cb retain:d];
    if ([(id)d isKindOfClass:[RMTexture class]])
        declareGpuWrites((RMTexture *)d);
    Cmd m = cmd(OpCopyBufferToTexture);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        so, row, image, size.width, size.height, size.depth, ds, dl, uint64_t(dorg.x) | uint64_t(dorg.y) << 32, dorg.z};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
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
                options:(MTLBlitOption)options
{
    if (!blitOptionsMoot(d, options)) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromBuffer:s
               sourceOffset:so
          sourceBytesPerRow:row
        sourceBytesPerImage:image
                 sourceSize:size
                  toTexture:d
           destinationSlice:ds
           destinationLevel:dl
          destinationOrigin:dorg];
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
    RMTexture *a = (RMTexture *)s;
    RMBuffer *b = (RMBuffer *)d;
    BufferRows rows;
    const bool ok =
        [(id)a isKindOfClass:[RMTexture class]] && [(id)b isKindOfClass:[RMBuffer class]] && !ss && size.depth == 1 &&
        copySideOk(a, sl, so.x, so.y, size.width, size.height) && a->_raw.bpp &&
        bufferRows(dof, row, uint32_t(size.height), uint32_t(size.width * a->_raw.bpp), a->_raw.bpp, false, rows) &&
        bufferRow(rows, dof, row, uint32_t(size.height - 1)) + size.width * a->_raw.bpp <= b->_raw.length;
    if (!ok) {
        [_cb takeReason:WhyTexture why:@"a texture-to-buffer copy the raw path does not do"];
        hit(_cb, PredBlitTexToBuf, row, uint64_t(ss) | uint64_t(sl) << 8 | uint64_t(size.depth) << 16);
    } else if (a->_raw.converted()) {
        [_cb takeReason:WhyTexture why:@"a copy to or from a planar texture"];
        hit(_cb, PredEncPlanarCopy, 1, 0);
    }
    [_cb retain:s];
    [_cb retain:d];
    Cmd m = cmd(OpCopyTextureToBuffer);
    m.obj = (__bridge void *)s;
    m.obj2 = (__bridge void *)d;
    m.x = _cb->_extra.size();
    const uint64_t e[10] = {
        ss, sl, uint64_t(so.x) | uint64_t(so.y) << 32, so.z, size.width, size.height, size.depth, dof, row, image};
    _cb->_extra.insert(_cb->_extra.end(), e, e + 10);
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
                     options:(MTLBlitOption)options
{
    if (!blitOptionsMoot(s, options)) {
        [_cb takeReason:WhyCall why:@"a copy with blit options"];
        hit(_cb, PredCallBlitOptions, options);
    }
    [self copyFromTexture:s
                     sourceSlice:ss
                     sourceLevel:sl
                    sourceOrigin:so
                      sourceSize:size
                        toBuffer:d
               destinationOffset:dof
          destinationBytesPerRow:row
        destinationBytesPerImage:image];
}

- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (x)
        declareGpuWrites(x);
    if (!x || !x->_rawOk || !x->_raw.chain) {  // a chain's levels are the 2D engine's (raw::BlitEncoder::generate)
        [_cb takeReason:WhyMipmaps why:@"generateMipmapsForTexture"];
        hit(_cb, PredMipmaps, t.pixelFormat, t.mipmapLevelCount);
    } else if (!x->_raw.chain->fmt2d) {  // m2v generates them, into the same chain (its in-place twin)
        [_cb takeReason:WhyMipmaps why:@"generateMipmapsForTexture of a format the 2D engine does not filter"];
        hit(_cb, PredMipmapsFormat, t.pixelFormat, t.mipmapLevelCount);
    }
    setBinding(_cb, OpMipmaps, 0, 0, t, 0);
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

@implementation RMComputeEncoder  // everything recorded as invocations (the command buffer is m2v's): see RMEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputeEncoder");
}

@end
