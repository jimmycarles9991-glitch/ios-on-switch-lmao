// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalRotated.h"

@implementation SwitchMetalBuffer

- (instancetype)init
{
    if ((self = [super init]))
        _identity = gm20bNewIdentity();
    return self;
}

- (void)dealloc
{
    void *bytes = _raw.cpu;
    const NSUInteger length = NSUInteger(_raw.length);
    if (_heap && !_raw.arena) {
        _heap->_used -= _heapBytes;
        gm20bHeapForget(_heap, _raw.va);
    }
    if (gRM)
        gRM->raw->destroyBuffer(_raw);
    if (_deallocator)
        _deallocator(bytes, length);
}

- (BOOL)invalidateForCpu
{
    if ((!_gpuDirty.load(std::memory_order_acquire) && !_raw.import) || _raw.wc)
        return YES;
    gm20b::HostAccess access;
    if (!gRM->raw->hostAccess(_raw, false, access) || !access.finish())
        return NO;
    _gpuDirty.store(false, std::memory_order_release);
    return YES;
}

- (void *)contents
{
    if (!_raw.hostVisible)
        return nullptr;
    return [self invalidateForCpu] ? _raw.cpu : nullptr;
}

- (NSUInteger)length
{
    return NSUInteger(_raw.length);
}

- (NSUInteger)heapOffset
{
    return _raw.arena && _raw.span.valid() ? NSUInteger(_raw.span.offset) : 0;
}

- (NSUInteger)allocatedSize
{
    return NSUInteger(_raw.span.valid() ? _raw.span.bytes : _raw.length);
}

- (uint64_t)gpuAddress
{
    return _raw.va;
}

- (MTLResourceOptions)resourceOptions
{
    return _options;
}

- (MTLStorageMode)storageMode
{
    return MTLStorageMode((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheMode((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return MTLHazardTrackingMode((_options & MTLResourceHazardTrackingModeMask) >> MTLResourceHazardTrackingModeShift);
}

- (void)didModifyRange:(NSRange)range
{
    (void)range;  // the draws that read it clean what they read (command_buffer.h CommandBuffer::clean)
}

// A buffer of its own keeps its pages while it lives: Volatile and Empty keep the contents (a later NonVolatile answers what
// was asked before, never Empty), which is what a caller that refills on Empty needs. A heap's buffer is its heap's (SwitchMetalHeap).
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (_heap)
        return [(id<MTLHeap>)_heap setPurgeableState:MTLPurgeableStateKeepCurrent];
    const MTLPurgeableState old = _purgeable ? _purgeable : MTLPurgeableStateNonVolatile;
    if (state != MTLPurgeableStateKeepCurrent)
        _purgeable = state == MTLPurgeableStateEmpty ? MTLPurgeableStateVolatile : state;
    return old;
}

- (id<MTLHeap>)heap
{
    return (id<MTLHeap>)_heap;
}

- (void)makeAliasable
{
    if (!_heap || _aliasable)
        return;
    _aliasable = true;
    if (_raw.arena)
        gRM->raw->makeArenaAliasable(_raw.span, _raw.lease);
    _heap->_used -= _heapBytes;
    _heapBytes = 0;
}

- (BOOL)isAliasable
{
    return _aliasable;
}

- (IOSurfaceRef)iosurface
{
    return nullptr;
}

@end

@implementation SwitchMetalBuffer (Textures)

// A texture over the buffer's bytes: the same memory under a texture header, or Metal's nil with one line; `linear`: Core
// Animation's linear SPI, which also takes a 2D array of slices `image` bytes apart
- (id<MTLTexture>)gm20bTextureWithDescriptor:(MTLTextureDescriptor *)d
                                      offset:(NSUInteger)offset
                                 bytesPerRow:(NSUInteger)row
                               bytesPerImage:(NSUInteger)image
                                      linear:(BOOL)linear
{
    if (refuseTurnedOffSurface(d))
        return nil;
    if (const char *why = gm20b::shapes::formatRefusal(uint32_t(d.pixelFormat))) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.buf.shape", d.pixelFormat,
                             "no texture of format %lu over a buffer: %s (MTLBuffer.h "
                             "newTextureWithDescriptor:offset:bytesPerRow:, nullable)",
                             (unsigned long)d.pixelFormat, why);
        return nil;
    }
    const bool linearArray = linear && d.textureType == MTLTextureType2DArray;
    SwitchMetalTexture *t = [SwitchMetalTexture new];
    t->_device = _device;
    t->_descriptor = [d copy];
    t->_kind = SwitchMetalTextureBuffer;
    t->_buffer = self;
    t->_offset = offset;
    t->_rowBytes = row;
    if (d.textureType == MTLTextureTypeTextureBuffer)
        t->_gm20bOk = d.width <= UINT32_MAX &&
                      gRM->raw->createTexelBuffer(_raw, offset, uint32_t(d.width), uint32_t(d.pixelFormat), t->_raw);
    else if (linearArray && d.mipmapLevelCount == 1 && d.sampleCount == 1 &&
             d.storageMode != MTLStorageModeMemoryless && d.arrayLength <= 2048 && image == uint64_t(row) * d.height)
        t->_gm20bOk = gRM->raw->createBufferTexture(_raw, offset, uint32_t(d.width), uint32_t(d.height), uint32_t(row),
                                                    uint32_t(d.pixelFormat), t->_raw, uint32_t(d.arrayLength));
    else if (gm20bShape(d) && d.storageMode != MTLStorageModeMemoryless)
        t->_gm20bOk = gRM->raw->createBufferTexture(_raw, offset, uint32_t(d.width), uint32_t(d.height), uint32_t(row),
                                                    uint32_t(d.pixelFormat), t->_raw);
    if (t->_gm20bOk && d.textureType != MTLTextureTypeTextureBuffer && !keepExtendedRange(d, t->_raw, true)) {
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: no memory or texture header for the stand-in of a %lux%lu format %lu "
                     "texture over a buffer: nil",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
        gRM->raw->destroyTexture(t->_raw);
        t->_gm20bOk = false;
        return nil;
    }
    if (!t->_gm20bOk) {  // which term (createBufferTexture's, recomputed: not the hot path)
        const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0);
        NSString *extra = [NSString stringWithFormat:@"offset %lu bytesPerRow %lu buffer %llu", (unsigned long)offset,
                                                     (unsigned long)row, (unsigned long long)_raw.length];
        if (d.textureType == MTLTextureTypeTextureBuffer) {
            if ((_raw.va + offset) & 15) {
                // a texel buffer's offset is a multiple of 16 (NVK's minTexelBufferOffsetAlignment): Metal's validation
                // stops any other
                os_log_error(gm20bLog(),
                             "SwitchMetal: %{public}s: no texel buffer over a buffer: offset off 16: %{public}@",
                             getprogname(), extra);
                return nil;
            }
            // Metal's validation stops a block-compressed texel buffer, one of no texels or past 2^27 of them
            // (maxTextureBufferWidth) and one past its buffer (an RGB10A8_2P_XR10 one's alpha bytes 16 KiB past its words:
            // xr.h texelBufferBytes); the rest is a header, or an extended-range one's stand-in, there is no room for
            os_log_error(
                gm20bLog(),
                "SwitchMetal: %{public}s: no %lu-texel buffer of format %lu: compressed, empty, past 2^27 texels or "
                "its buffer, or no texture header or stand-in memory left: nil: %{public}@",
                getprogname(), (unsigned long)d.width, (unsigned long)d.pixelFormat, extra);
            return nil;
        } else if (linearArray && image != uint64_t(row) * d.height) {
            // CA's newLinearTextureWithDescriptor: of a 2D array (m2v linear_plan.rs: slices bytesPerImage apart): GM20B's
            // pitch header has no array stride, its layers lie its rows' bytes times its height apart
            SWITCH_REFUSE_DETAIL("gm20b", "tex.buf.array_stride", image,
                                 "no %lux%lux%lu linear array of format %lu over a buffer with slices %lu bytes apart: "
                                 "the pitch header lays them %llu apart (clb097tex.h TEXHEAD_PITCH has no array "
                                 "stride): nil: %s",
                                 (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.arrayLength,
                                 (unsigned long)d.pixelFormat, (unsigned long)image, (unsigned long long)row * d.height,
                                 extra.UTF8String);
            return nil;
        } else if (const uint32_t term = linearArray ? 0 : gm20bShapeTerm(d)) {
            // m2v linear_plan.rs, measured: a texture over a buffer is a 2D texture or a texture buffer, of one level,
            // never two-plane extended range; a format raw keeps none of is Metal's or GM20B's refusal at creation too
            SWITCH_REFUSE_DETAIL(
                "gm20b", "tex.buf.shape", term,
                "no %lux%lu texture of format %lu, type %lu, %lu levels over a buffer (term %u: a type, "
                "level count or format Metal makes no texture over a buffer of): nil (MTLBuffer.h "
                "newTextureWithDescriptor:offset:bytesPerRow:, nullable): %s",
                (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, term, extra.UTF8String);
            return nil;
        } else if (((_raw.va + offset) & 15) || (row & 15) || row < packed) {
            // a 2D texture's offset and rows are multiples of 16 (m2v linear_plan.rs, measured): Metal's validation stops
            // any other, which m2v refused
            os_log_error(
                gm20bLog(),
                "SwitchMetal: %{public}s: no texture over a buffer: offset or rows off 16, or short: %{public}@",
                getprogname(), extra);
            return nil;
        } else if (linearArray && (((_raw.va + offset) | row) & 31)) {
            // the pitch header addresses rows and a start of 32 bytes; a 2D texture off that is sampled through a pool
            // image of its texels, which no layered pitch header has
            SWITCH_REFUSE_DETAIL("gm20b", "tex.buf.array_stride", row,
                                 "no linear array over a buffer of rows or a start off 32 bytes (clb097tex.h "
                                 "TEXHEAD_PITCH PITCH_BITS20TO5, ADDRESS_BITS31TO5): nil: %s",
                                 extra.UTF8String);
            return nil;
        } else if (d.storageMode == MTLStorageModeMemoryless || offset + uint64_t(row) * d.height > _raw.length) {
            os_log_error(gm20bLog(),
                         "SwitchMetal: %{public}s: no texture over a buffer: memoryless, or past its end: %{public}@",
                         getprogname(), extra);
            return nil;
        } else {
            // Metal's validation stops a block-compressed or oversized linear texture; the rest is the memory of the pool
            // image rows off 32 bytes are sampled through, or a header the table has no room for
            os_log_error(gm20bLog(),
                         "SwitchMetal: %{public}s: no %lux%lu texture of format %lu over a buffer: compressed, past "
                         "16384 texels, "
                         "or no memory or texture header for it: nil: %{public}@",
                         getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         extra);
            return nil;
        }
    }
    return swizzled((id<MTLTexture>)t, d.swizzle);
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    return [self gm20bTextureWithDescriptor:d offset:offset bytesPerRow:row bytesPerImage:0 linear:NO];
}

// SPI (Core Animation): the same, or a 2D array
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    return [self gm20bTextureWithDescriptor:d offset:offset bytesPerRow:row bytesPerImage:image linear:YES];
}

@end
