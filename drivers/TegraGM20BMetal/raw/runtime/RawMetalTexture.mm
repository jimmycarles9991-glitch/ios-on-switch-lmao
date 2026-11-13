// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalMultisample.h"
#include "raw_texture_cpu_range.h"

namespace {
class TextureCpuAccess {
public:
    TextureCpuAccess(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, bool write)
    {
        if (!texture->_rawOk || !texture->_raw.cpu)
            return;
        const auto &image = texture->_raw;
        if (image.blockW > 1 || image.blockH > 1)
            return;
        const raw::TextureCpuShape shape = {image.width,
                                            image.height,
                                            image.depth,
                                            image.layers,
                                            image.pitch,
                                            image.surfaceX,
                                            image.surfaceY,
                                            image.bpp,
                                            image.blockDepth,
                                            image.blockLinear,
                                            image.type == raw::fx::kTexThreeD,
                                            image.chain ? &image.chain->layout : nullptr,
                                            textureCpuImportLayout(texture),
                                            image.layerBytes};
        const uint64_t origin[] = {region.origin.x, region.origin.y, region.origin.z};
        const uint64_t size[] = {region.size.width, region.size.height, region.size.depth};
        raw::TextureCpuRange range;
        const auto planned = raw::textureCpuRange(shape, origin, size, level, slice, range);
        if (planned == raw::TextureCpuRegion::Unsupported)
            return;
        if (planned != raw::TextureCpuRegion::Native || range.offset > UINTPTR_MAX - uintptr_t(image.cpu))
            [NSException raise:NSInvalidArgumentException format:@"GPU texture CPU region is outside its image"];
        const void *cpu = reinterpret_cast<const void *>(uintptr_t(image.cpu) + range.offset);
        while (texture->_kind == RMTextureView && texture->_parent)
            texture = texture->_parent;
        const bool acquired = texture->_buffer
                                  ? gRM->raw->hostAccess(texture->_buffer->_raw, cpu, range.bytes, write, access_)
                                  : gRM->raw->hostAccess(texture->_raw, cpu, range.bytes, write, access_);
        if (!acquired)
            [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
        native_ = true;
    }

    bool native() const
    {
        return native_;
    }

private:
    raw::HostAccess access_;
    bool native_ = false;
};
}  // namespace

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
    if (_heap && !_raw.arena) {
        _heap->_used -= _heapBytes;
        const raw::Texture::StorageRange storage = _raw.storageRange();
        rmHeapForget(_heap, storage.bytes ? storage.va : _raw.va);
    }
    if (_rawOk && _surface && _raw.converted())
        surfaceConverted(_surfaceWrites, false);
    rmHeapForgetTexture(self);
    if (_rawOk)
        if (gRM)
            gRM->raw->destroyTexture(_raw);
    if (_surface) {
        releaseSurfaceWrites(_surface, _surfaceWrites);
        CFRelease(_surface);
    }
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    id<MTLDevice> m2v = RM_M2V("a texture's twin");
    id made = nil;  // outside this object's lock (twinPublish)
    const char *why = "m2v made none";
    {
        switch (_kind) {
        case RMTextureSurface:
            made = [m2v newTextureWithDescriptor:_descriptor iosurface:_surface plane:_plane];
            break;
        case RMTexturePool: {  // m2v's in-place texture over the same pool memory (as an IOSurface's)
            rmTexturePlain(self);
            // zeta memory of its own (raw::Device::createOwnedZeta): only a range that keeps none of it replays (ownedZetaIn)
            if (_raw.zeta && (_raw.samples > 1 || _raw.arena)) {
                made = [m2v newTextureWithDescriptor:_descriptor];
                break;
            }
            if (zetaDescriptor(
                    _descriptor)) {  // a stored one-sample zeta, moved into the twin's image (storedZetaTwin)
                made = _rawOk ? storedZetaTwin(self, m2v, &why) : [m2v newTextureWithDescriptor:_descriptor];
                break;
            }
            const size_t page = size_t(getpagesize());
            raw::Alias a;
            // Made aliasable before its first replay: its placement is given back, its address and its heap's arena kept
            const bool released = _raw.arena && !_raw.span.valid() && _heap && _heap->_arena;
            uint8_t *cpu = released ? _raw.cpu : _raw.span.cpu;
            uint64_t bytes = _raw.span.bytes, align;
            const auto releasedBytes = [&] {
                raw::TextureAsk ask;
                if (!heapTextureAsk(_descriptor, _heap->_descriptor.storageMode == MTLStorageModePrivate, ask))
                    return false;
                ask.standIn = false;
                return raw::Device::footprint(ask, bytes, align);
            };
            const bool shared =
                heapResident(_heap) &&
                (released ? releasedBytes() && gRM->raw->aliasArenaRange(_heap->_arena - 1, _raw.va, bytes, a)
                          : gRM->raw->alias(_raw, a));
            id<MTLBuffer> pages = shared ? rawAliasBuffer(a, cpu, (NSUInteger(bytes) + page - 1) & ~(page - 1),
                                                          MTLResourceStorageModeShared)
                                         : nil;
            why = _raw.zeta && _raw.samples > 1     ? "NVK views no multisampled zeta memory the raw path owns"
                  : !_raw.span.valid() && !released ? "it has no memory of its own"
                  : !shared ? "its memory could not be aliased (a purged heap's that did not come back)"
                  : !pages  ? "m2v did not take its memory"
                            : "m2v made no texture over its memory";
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
    const char *error = !made && nmb_last_error ? nmb_last_error() : nullptr;
    id twin = twinPublish(self, _twin, made);
    if (!twin)
        SWITCH_REFUSE_DETAIL("raw", "twin.texture", _descriptor.pixelFormat,
                             "no m2v twin for a %ux%u texture of format %lu (kind %u%s): %s (%s)", _raw.width,
                             _raw.height, (unsigned long)_descriptor.pixelFormat, unsigned(_kind),
                             _raw.arena ? ", in a heap" : "", why, error && *error ? error : "-");
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
    return twin ? [(id<MTLTexture>)twin allocatedSize] : NSUInteger(_raw.storage.bytes);
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

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    if (!r.size.width || !r.size.height || !r.size.depth)
        return;
#if SWITCH_RAW_DIAGNOSTICS
    _raw.probeWritten = true;
#endif
    if (_surfaceWrites)
        noteSurfaceWrite(_surfaceWrites);
    if (transferTexture(self, r, level, slice, const_cast<void *>(bytes), row, image, true))
        return;
    if (_rawOk && _raw.blockW * _raw.blockH > 1 && _raw.blockLinear) {
        cpuBlocks(self, r, level, slice, (uint8_t *)bytes, row, true);
        return;
    }
    TextureCpuAccess access(self, r, level, slice, true);
    if (!access.native()) {
        [(id<MTLTexture>)[self rmTwin] replaceRegion:r
                                         mipmapLevel:level
                                               slice:slice
                                           withBytes:bytes
                                         bytesPerRow:row
                                       bytesPerImage:image];
        return;
    }
    if (_rawOk && _raw.chain) {
        cpuLevel(self, r, level, slice, (uint8_t *)bytes, row, image, true);
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
    }
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    if (!r.size.width || !r.size.height || !r.size.depth)
        return;
    if (transferTexture(self, r, level, slice, bytes, row, image, false))
        return;
    if (_rawOk && _raw.blockW * _raw.blockH > 1 && _raw.blockLinear) {
        cpuBlocks(self, r, level, slice, (uint8_t *)bytes, row, false);
        return;
    }
    TextureCpuAccess access(self, r, level, slice, false);
    if (!access.native()) {
        [(id<MTLTexture>)[self rmTwin] getBytes:bytes
                                    bytesPerRow:row
                                  bytesPerImage:image
                                     fromRegion:r
                                    mipmapLevel:level
                                          slice:slice];
        return;
    }
    if (_rawOk && _raw.chain) {
        cpuLevel(self, r, level, slice, (uint8_t *)bytes, row, image, false);
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
    if (r.origin.x + r.size.width > _raw.width || r.origin.y + r.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, false);
        return;
    }
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
    const NSUInteger allSlices = raw::textureLayers(uint32_t(_descriptor.textureType),
                                                    uint32_t(std::min<NSUInteger>(_descriptor.arrayLength, 2048)));
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
    rmTexturePlain(self);
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
    rmTexturePlain(self);
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
