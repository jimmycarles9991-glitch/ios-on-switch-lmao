// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// Before the raw path first renders into a surface texture: the kernel takes the surface's imports as only sampled until
// then, so a CPU lock of a surface the GPU never writes invalidates nothing.
void declareGpuWrites(RMTexture *t)
{
    if (!t->_surface || t->_gpuWrites.exchange(true))
        return;
    if (gRM && gRM->raw && gRM->raw->surfaceGpuWrites(IOSurfaceGetID(t->_surface)))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log_error(rawLog(),
                     "raw: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(t->_surface));
}

#if SWITCH_FRAME_TIMELINE
// A YCbCr or planar surface's image, made from the surface's planes by GPU work at the seed it had then: when its texture
// is made and at each conversion, with the seed before (none: never converted). The kernel's IOSurface lines name the
// CPU locks and writing unlocks of the same surface ID around it; a seed that moves with no unlock there is a writer the
// locks never see (a hardware engine, or a CPU write without a lock).
void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed)
{
    if (!t->_surface)
        return;
    const OSType f = IOSurfaceGetPixelFormat(t->_surface);
    os_log(rawLog(), "raw: IOSurface %u %ux%u '%c%c%c%c' image %s at seed %u (before: %s%u)",
           IOSurfaceGetID(t->_surface), unsigned(IOSurfaceGetWidth(t->_surface)),
           unsigned(IOSurfaceGetHeight(t->_surface)), char(f >> 24), char(f >> 16), char(f >> 8), char(f), what, seed,
           t->_imageCurrent ? "" : "none ", t->_imageCurrent ? t->_imageSeed : 0u);
}
#endif

@implementation RMTexture

+ (Class)twinClass
{
    return NSClassFromString(@"NBTexture");
}

// SPI: the descriptor's (0 for every texture the raw path keeps, rawShape)
- (NSUInteger)rotation
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [_descriptor respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(_descriptor, rotation)
                                                     : 0;
}

- (void)dealloc
{
    _twin = nil;
    if (_heap && !_raw.arena)
        _heap->_used -= _heapBytes;
    if (_rawOk && _surface && _raw.converted())
        surfaceConverted(_surface, false);
    if (_rawOk)
        if (gRM)
            gRM->raw->destroyTexture(_raw);
    if (_surface)
        CFRelease(_surface);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    id<MTLDevice> m2v = gRM->fallback;
    id made = nil;  // outside this object's lock (twinPublish)
    {
        switch (_kind) {
        case RMTextureSurface:
            made = [m2v newTextureWithDescriptor:_descriptor iosurface:_surface plane:_plane];
            break;
        case RMTexturePool: {  // m2v's in-place texture over the same pool memory (as an IOSurface's)
            const size_t page = size_t(getpagesize());
            raw::Alias a;
            id<MTLBuffer> pages =
                gRM->raw->alias(_raw, a)
                    ? rawAliasBuffer(a, _raw.span.cpu, (NSUInteger(_raw.span.bytes) + page - 1) & ~(page - 1),
                                     MTLResourceStorageModeShared)
                    : nil;
            MTLTextureDescriptor *d = _descriptor;
            if (_raw.chain && d.storageMode == MTLStorageModeMemoryless) {  // every level, in place (memory here)
                d = [_descriptor copy];
                d.storageMode = MTLStorageModePrivate;
            }
            if (pages && _raw.blockLinear && native_bridge_metal_texture_tiled_in_place)
                made = native_bridge_metal_texture_tiled_in_place(pages, d, 0);
            else if (pages && !_raw.blockLinear && native_bridge_metal_texture_in_place)
                made = native_bridge_metal_texture_in_place(pages, d, 0, _raw.pitch);
            break;
        }
        case RMTextureBuffer:
            if (id<MTLBuffer> b = [_buffer rmTwin]) {
                made = native_bridge_metal_texture_in_place
                           ? native_bridge_metal_texture_in_place(b, _descriptor, _offset, _rowBytes)
                           : [b newTextureWithDescriptor:_descriptor offset:_offset bytesPerRow:_rowBytes];
            }
            break;
        case RMTextureView:
            made = _viewPart ? [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat
                                                                                   textureType:_descriptor.textureType
                                                                                        levels:_viewLevels
                                                                                        slices:_viewSlices]
                             : [(id<MTLTexture>)[_parent rmTwin] newTextureViewWithPixelFormat:_descriptor.pixelFormat];
            if (made && !identitySwizzle(_descriptor.swizzle))  // m2v's texture makes no swizzled view
                SWITCH_REFUSE_DETAIL("raw", "tex.swizzle", _descriptor.pixelFormat,
                                     "a swizzled view of format %lu: m2v's work samples it unswizzled",
                                     (unsigned long)_descriptor.pixelFormat);
            break;
        case RMTextureMemoryless:
            made = [m2v newTextureWithDescriptor:_descriptor];
            break;
        case RMTextureTwinOnly:
            break;
        }
    }
    id twin = twinPublish(self, _twin, made);
    if (!twin)
        SWITCH_REFUSE_DETAIL("raw", "twin.texture", _descriptor.pixelFormat,
                             "no m2v twin for a %ux%u texture of format %lu (kind %u)", _raw.width, _raw.height,
                             (unsigned long)_descriptor.pixelFormat, unsigned(_kind));
    return twin;
}

// Answered here, not by the twin (forwarded, each would make one): as m2v answers them
- (MTLTextureCompressionType)compressionType
{
    return _descriptor.compressionType;
}

- (BOOL)isCompressed
{
    return NO;
}

- (BOOL)isDrawable
{
    return NO;
}

- (MTLTextureSwizzleChannels)swizzle
{
    return _descriptor.swizzle;
}

- (NSUInteger)heapOffset
{
    return _raw.arena && _raw.span.valid() ? NSUInteger(_raw.span.offset) : 0;
}

// Answered here, not by the twin (a residency set's allocatedSize sums its allocations'): the memory this texture owns. A view and
// a texture over a buffer own none; a twin-only texture's is its twin's.
- (NSUInteger)allocatedSize
{
    if (_kind == RMTextureView || _kind == RMTextureBuffer || _raw.memoryless)
        return 0;
    if (_surface)
        return IOSurfaceGetAllocSize(_surface);
    if (_raw.span.valid())
        return NSUInteger(_raw.span.bytes);
    id twin = _twin;
    return twin ? [(id<MTLTexture>)twin allocatedSize] : 0;
}

- (MTLTextureType)textureType
{
    return _descriptor.textureType;
}

- (MTLPixelFormat)pixelFormat
{
    return _descriptor.pixelFormat;
}

- (NSUInteger)width
{
    return _descriptor.width;
}

- (NSUInteger)height
{
    return _descriptor.height;
}

- (NSUInteger)depth
{
    return _descriptor.depth;
}

- (NSUInteger)mipmapLevelCount
{
    return _descriptor.mipmapLevelCount;
}

- (NSUInteger)arrayLength
{
    return _descriptor.arrayLength;
}

- (NSUInteger)sampleCount
{
    return _descriptor.sampleCount;
}

- (MTLTextureUsage)usage
{
    return _descriptor.usage;
}

- (MTLStorageMode)storageMode
{
    return _descriptor.storageMode;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return _descriptor.cpuCacheMode;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return _descriptor.hazardTrackingMode;
}

- (MTLResourceOptions)resourceOptions
{
    return _descriptor.resourceOptions;
}

- (BOOL)isFramebufferOnly
{
    return NO;
}

- (BOOL)allowGPUOptimizedContents
{
    return _descriptor.allowGPUOptimizedContents;
}

- (IOSurfaceRef)iosurface
{
    return _surface;
}

- (NSUInteger)iosurfacePlane
{
    return _plane;
}

- (id<MTLTexture>)parentTexture
{
    return (id<MTLTexture>)_parent;
}

- (id<MTLBuffer>)buffer
{
    return (id<MTLBuffer>)_buffer;
}

- (NSUInteger)bufferOffset
{
    return _offset;
}

- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}

- (NSUInteger)parentRelativeLevel
{
    return 0;
}

- (NSUInteger)parentRelativeSlice
{
    return 0;
}

- (BOOL)isShareable
{
    return NO;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (_heap)
        return [(id<MTLHeap>)_heap setPurgeableState:MTLPurgeableStateKeepCurrent];
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}

- (void)makeAliasable
{
    if (!_heap || _aliasable)
        return;
    _aliasable = true;
    if (_raw.arena)
        gRM->raw->arenaRelease(_raw);
    _heap->_used -= _heapBytes;
    _heapBytes = 0;
}

- (BOOL)isAliasable
{
    return _aliasable;
}

- (id<MTLHeap>)heap
{
    return (id<MTLHeap>)_heap;
}

// CPU writes into the texture's own pages (level 0, slice 0, a 2D texture), cleaned to memory at once: a later command buffer's
// GPU reads see them. A shared mipmapped texture's levels: cpuLevel. Other textures are m2v's (their twin's replaceRegion).
- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    if (_surface)
        noteSurfaceWrite(_surface);
    if (_rawOk && _raw.chain && !slice && r.size.depth <= 1) {
        cpuLevel(self, r, level, (uint8_t *)bytes, row, true);
        return;
    }
    if (_rawOk && _raw.type == raw::fx::kTexThreeD && !level && !slice && r.origin.x + r.size.width <= _raw.width &&
        r.origin.y + r.size.height <= _raw.height && r.origin.z + r.size.depth <= _raw.depth) {
        cpuVolume(self, r, (uint8_t *)bytes, row, image, true);
        return;
    }
    if (_rawOk && _raw.layers > 1 && !level && slice < _raw.layers && r.size.depth <= 1 &&
        r.origin.x + r.size.width <= _raw.width && r.origin.y + r.size.height <= _raw.height) {
        cpuLayer(self, r, slice, (uint8_t *)bytes, row, true);
        return;
    }
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] replaceRegion:r
                                         mipmapLevel:level
                                               slice:slice
                                           withBytes:bytes
                                         bytesPerRow:row
                                       bytesPerImage:image];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, true);
        return;
    }
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *to = _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp;
        memcpy(to, (const uint8_t *)bytes + y * row, line);
#if defined(__aarch64__)
        if (!_raw.wc)  // (write-combined: in memory once the DSB below drains it)
            for (uintptr_t a = uintptr_t(to) & ~uintptr_t(63); a < uintptr_t(to) + line; a += 64)
                __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
#endif
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}

// CPU reads of what the GPU wrote: its lines invalidated first (once per GPU write), then the rows
- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    if (_rawOk && _raw.chain && !slice && r.size.depth <= 1) {
        cpuLevel(self, r, level, (uint8_t *)bytes, row, false);
        return;
    }
    if (_rawOk && _raw.type == raw::fx::kTexThreeD && !level && !slice && r.origin.x + r.size.width <= _raw.width &&
        r.origin.y + r.size.height <= _raw.height && r.origin.z + r.size.depth <= _raw.depth) {
        cpuVolume(self, r, (uint8_t *)bytes, row, image, false);
        return;
    }
    if (_rawOk && _raw.layers > 1 && !level && slice < _raw.layers && r.size.depth <= 1 &&
        r.origin.x + r.size.width <= _raw.width && r.origin.y + r.size.height <= _raw.height) {
        cpuLayer(self, r, slice, (uint8_t *)bytes, row, false);
        return;
    }
    if (!_rawOk || level || slice || r.size.depth > 1 || !_raw.cpu) {
        [(id<MTLTexture>)[self rmTwin] getBytes:bytes
                                    bytesPerRow:row
                                  bytesPerImage:image
                                     fromRegion:r
                                    mipmapLevel:level
                                          slice:slice];
        return;
    }
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, false);
        return;
    }
#if defined(__aarch64__)
    if (_gpuDirty.exchange(false) && !_raw.wc) {
        for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63);
             a < uintptr_t(_raw.cpu) + uint64_t(_raw.pitch) * _raw.height; a += 64)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif
    const size_t line = r.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < r.size.height; y++)
        memcpy((uint8_t *)bytes + y * row, _raw.cpu + (r.origin.y + y) * _raw.pitch + r.origin.x * _raw.bpp, line);
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}

bool identitySwizzle(MTLTextureSwizzleChannels s)
{
    return s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen && s.blue == MTLTextureSwizzleBlue &&
           s.alpha == MTLTextureSwizzleAlpha;
}

id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s)
{
    RMTexture *x = [(id)t isKindOfClass:[RMTexture class]] ? (RMTexture *)t : nil;
    if (!x || identitySwizzle(s))
        return t;
    x->_descriptor.swizzle = s;
    const uint8_t channels[4] = {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)};
    if (x->_rawOk)
        gRM->raw->swizzle(x->_raw, channels);
    return t;
}

// The header type of a view of `type` (-1: one the raw path makes none of)
static int viewHeaderType(MTLTextureType type)
{
    switch (type) {
    case MTLTextureType1D:
        return raw::fx::kTexOneD;
    case MTLTextureType1DArray:
        return raw::fx::kTexOneDArray;
    case MTLTextureType2D:
        return raw::fx::kTexTwoD;
    case MTLTextureType2DArray:
        return raw::fx::kTexTwoDArray;
    case MTLTextureTypeCube:
        return raw::fx::kTexCube;
    case MTLTextureTypeCubeArray:
        return raw::fx::kTexCubeArray;
    default:
        return -1;
    }
}

// A view: the same memory under another format's header (raw), or m2v's view of the twin
// A view of all of it, as its own type: the format view. Of a part of it, or another type: a header over that part
// (raw::Device::createPartView), else m2v's view of the twin, which raw command buffers refuse (tex.view, b 3)
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
{
    const bool cube =
        _descriptor.textureType == MTLTextureTypeCube || _descriptor.textureType == MTLTextureTypeCubeArray;
    const NSUInteger allSlices = std::max<NSUInteger>(_descriptor.arrayLength, 1) * (cube ? 6 : 1);
    if (type == _descriptor.textureType && !levels.location && levels.length == _descriptor.mipmapLevelCount &&
        !slices.location && slices.length == allSlices)
        return [self newTextureViewWithPixelFormat:format];
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_descriptor.textureType = type;
    v->_descriptor.mipmapLevelCount = levels.length;
    v->_descriptor.width = std::max<NSUInteger>(_descriptor.width >> levels.location, 1);
    v->_descriptor.height = std::max<NSUInteger>(_descriptor.height >> levels.location, 1);
    if (type == MTLTextureType3D)
        v->_descriptor.depth = std::max<NSUInteger>(_descriptor.depth >> levels.location, 1);
    const bool viewCube = type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray;
    v->_descriptor.arrayLength = std::max<NSUInteger>(viewCube ? slices.length / 6 : slices.length, 1);
    v->_kind = RMTextureView;
    v->_parent = self;
    v->_viewPart = true;
    v->_viewLevels = levels;
    v->_viewSlices = slices;
    const int header = viewHeaderType(type);
    if (_rawOk && header >= 0 && raw::formatInfo(uint32_t(format)))
        v->_rawOk = gRM->raw->createPartView(_raw, uint32_t(format), uint32_t(header), uint32_t(levels.location),
                                             uint32_t(levels.length), uint32_t(slices.location),
                                             uint32_t(slices.length), v->_raw);
    if (v->_rawOk)
        return (id<MTLTexture>)v;
    if (![v rmTwin])
        return nil;
    refuseTexture(v, PredTexView, format, 3, @"view of a part",
                  [NSString stringWithFormat:@"type %lu levels %lu+%lu slices %lu+%lu", (unsigned long)type,
                                             (unsigned long)levels.location, (unsigned long)levels.length,
                                             (unsigned long)slices.location, (unsigned long)slices.length]);
    return (id<MTLTexture>)v;
}

// A swizzled view: the view, its header reading its channels so. m2v's texture answers nil to this, so a view the raw path does
// not hold is m2v's unswizzled one, said so.
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
                                        swizzle:(MTLTextureSwizzleChannels)swizzle
{
    RMTexture *v = (RMTexture *)[self newTextureViewWithPixelFormat:format
                                                        textureType:type
                                                             levels:levels
                                                             slices:slices];
    if (v && !v->_rawOk && !identitySwizzle(swizzle))
        SWITCH_REFUSE_DETAIL("raw", "tex.swizzle", format,
                             "a swizzled view of format %lu, type %lu, over a texture the raw path does not hold: m2v "
                             "samples it unswizzled",
                             (unsigned long)format, (unsigned long)type);
    return swizzled((id<MTLTexture>)v, swizzle);
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    RMTexture *v = [RMTexture new];
    v->_device = _device;
    v->_descriptor = [_descriptor copy];
    v->_descriptor.pixelFormat = format;
    v->_kind = RMTextureView;
    v->_parent = self;
    if (_rawOk && !_raw.memoryless && raw::formatInfo(uint32_t(format)))
        v->_rawOk = gRM->raw->createView(_raw, uint32_t(format), v->_raw);
    if (!v->_rawOk)
        refuseTexture(v, PredTexView, format, _rawOk, @"view",
                      [NSString stringWithFormat:@"parent kind %u pred %s", unsigned(_kind), kPredNames[_pred]]);
    if (!v->_rawOk && ![v rmTwin])
        return nil;
    return (id<MTLTexture>)v;
}

@end
