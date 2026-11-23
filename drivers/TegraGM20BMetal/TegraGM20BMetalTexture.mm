// SPDX-License-Identifier: MIT
// A raw texture: its memory's lifetime, and what Metal answers of it
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalFresh.h"
#import "TegraGM20BMetalDecoded.h"

// Before the driver first renders into a surface texture: the kernel takes the surface's imports as only sampled until
// then, so a CPU lock of a surface the GPU never writes invalidates nothing.
void declareGpuWrites(SwitchMetalTexture *t)
{
    // every pass over a surface target asks: only the first may exchange
    if (!t->_surface || t->_gpuWrites.load(std::memory_order_acquire) || t->_gpuWrites.exchange(true))
        return;
    if (gRM && gRM->raw && gRM->raw->surfaceGpuWrites(IOSurfaceGetID(t->_surface)))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log_error(gm20bLog(),
                     "SwitchMetal: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(t->_surface));
}

#if SWITCH_FRAME_TIMELINE
// A YCbCr or planar surface's image, made from the surface's planes by GPU work at the seed it had then: when its texture
// is made and at each conversion, with the seed before (none: never converted). The kernel's IOSurface lines name the
// CPU locks and writing unlocks of the same surface ID around it; a seed that moves with no unlock there is a writer the
// locks never see (a hardware engine, or a CPU write without a lock).
void traceSurfaceImage(SwitchMetalTexture *t, const char *what, uint32_t seed)
{
    if (!t->_surface)
        return;
    const OSType f = IOSurfaceGetPixelFormat(t->_surface);
    os_log(gm20bLog(), "SwitchMetal: IOSurface %u %ux%u '%c%c%c%c' image %s at seed %u (before: %s%u)",
           IOSurfaceGetID(t->_surface), unsigned(IOSurfaceGetWidth(t->_surface)),
           unsigned(IOSurfaceGetHeight(t->_surface)), char(f >> 24), char(f >> 16), char(f >> 8), char(f), what, seed,
           t->_imageCurrent ? "" : "none ", t->_imageCurrent ? t->_imageSeed : 0u);
}
#endif

@implementation SwitchMetalTexture

- (instancetype)init
{
    if ((self = [super init]))
        _identity = gm20bNewIdentity();
    return self;
}

// SPI: the descriptor's (0 for every texture the driver keeps, gm20bShape)
- (NSUInteger)rotation
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [_descriptor respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(_descriptor, rotation)
                                                     : 0;
}

- (void)dealloc
{
    if (_ownForm)
        gm20bForgetForm(self);
    if (_heap && !_raw.arena) {
        _heap->_used -= _heapBytes;
        const gm20b::Texture::StorageRange storage = _raw.storageRange();
        gm20bHeapForget(_heap, storage.bytes ? storage.va : _raw.va);
    }
    if (_gm20bOk && _surface && _raw.converted())
        surfaceConverted(_surfaceWrites, false);
    gm20bHeapForgetTexture(self);
    if (_gm20bOk)
        if (gRM)
            gRM->raw->destroyTexture(_raw);
    if (_surface) {
        releaseSurfaceWrites(_surface, _surfaceWrites);
        CFRelease(_surface);
    }
}

// Answered here: as Metal answers them
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

// The memory this texture owns (a residency set's allocatedSize sums its allocations'). A view and a texture over a buffer own
// none.
- (NSUInteger)allocatedSize
{
    if (_kind == SwitchMetalTextureView || _kind == SwitchMetalTextureBuffer || _raw.memoryless)
        return 0;
    if (_surface)
        return IOSurfaceGetAllocSize(_surface);
    if (_movableBytes)
        return NSUInteger(_movableBytes);
    if (_raw.span.valid())
        return NSUInteger(_raw.span.bytes);
    return NSUInteger(_raw.storage.bytes);
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
        gRM->raw->makeArenaAliasable(_raw);
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

bool identitySwizzle(MTLTextureSwizzleChannels s)
{
    return s.red == MTLTextureSwizzleRed && s.green == MTLTextureSwizzleGreen && s.blue == MTLTextureSwizzleBlue &&
           s.alpha == MTLTextureSwizzleAlpha;
}

id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s)
{
    SwitchMetalTexture *x = [(id)t isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)t : nil;
    if (!x || identitySwizzle(s))
        return t;
    x->_descriptor.swizzle = s;
    const uint8_t channels[4] = {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)};
    if (x->_gm20bOk)
        gRM->raw->swizzle(x->_raw, channels);
    return t;
}

@end
