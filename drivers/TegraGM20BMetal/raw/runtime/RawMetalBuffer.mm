// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

@implementation RMBuffer

+ (Class)twinClass
{
    return NSClassFromString(@"NBBuffer");
}

- (void)dealloc
{
    _twin = nil;  // m2v's import over these pages goes first
    void *bytes = _raw.cpu;
    const NSUInteger length = NSUInteger(_raw.length);
    if (_heap && !_raw.arena) {
        _heap->_used -= _heapBytes;
        rmHeapForget(_heap, _raw.va);
    }
    if (gRM)
        gRM->raw->destroyBuffer(_raw);
    if (_deallocator)
        _deallocator(bytes, length);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    // made outside this object's lock (twinPublish): in place over the same memory (a pool span or the caller's no-copy pages),
    // Shared whatever this buffer's storage mode (the memory is CPU-visible; m2v makes no Private buffer over host memory)
    // (a no-copy buffer's own length: its pages are the kernel's around it, the twin's no more than the import holds)
    const size_t page = size_t(getpagesize());
    const MTLResourceOptions options = (_options & ~MTLResourceStorageModeMask) | MTLResourceStorageModeShared;
    const NSUInteger length =
        _raw.import ? NSUInteger(_raw.length) : (NSUInteger(_raw.length) + page - 1) & ~(page - 1);
    raw::Alias a;
    return twinPublish(self, _twin,
                       RM_M2V("a buffer's twin") && heapResident(_heap) && gRM->raw->alias(_raw, a)
                           ? rawAliasBuffer(a, _raw.cpu, length, options)
                           : nil);
}

- (BOOL)invalidateForCpu
{
    if ((!_gpuDirty.load(std::memory_order_acquire) && !_raw.import) || _raw.wc)
        return YES;
    raw::HostAccess access;
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
    (void)range;  // the draws that read it clean what they read (raw_gpu.cpp CommandBuffer::clean)
}

// A buffer of its own keeps its pages while it lives: Volatile and Empty keep the contents (a later NonVolatile answers what
// was asked before, never Empty), which is what a caller that refills on Empty needs. A heap's buffer is its heap's (RMHeap).
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
        gRM->raw->arenaRelease(_raw);
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

@implementation RMBuffer (Textures)

// A texture over the buffer's bytes: the same memory under a texture header (raw), or m2v's in-place texture over the twin
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    RMTexture *t = [RMTexture new];
    t->_device = _device;
    t->_descriptor = [d copy];
    t->_kind = RMTextureBuffer;
    t->_buffer = self;
    t->_offset = offset;
    t->_rowBytes = row;
    if (rawShape(d) && d.storageMode != MTLStorageModeMemoryless)
        t->_rawOk = gRM->raw->createBufferTexture(_raw, offset, uint32_t(d.width), uint32_t(d.height), uint32_t(row),
                                                  uint32_t(d.pixelFormat), t->_raw);
    if (t->_rawOk)
        keepExtendedRange(d, t->_raw, true);
    if (!t->_rawOk) {  // which term (createBufferTexture's, recomputed: not the hot path)
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0), at = (_raw.va + offset) & 31;
        NSString *extra = [NSString stringWithFormat:@"offset %lu bytesPerRow %lu buffer %llu", (unsigned long)offset,
                                                     (unsigned long)row, (unsigned long long)_raw.length];
        if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"buffer", extra);
        else if (at)
            refuseTexture(t, PredTexBufBase, at, offset, @"buffer", extra);
        else if ((row & 31) || row < packed)
            refuseTexture(t, PredTexBufPitch, row, packed, @"buffer", extra);
        else
            refuseTexture(t, PredTexBufOther, offset + uint64_t(row) * d.height, _raw.length, @"buffer", extra);
    }
    if (!t->_rawOk && ![t rmTwin])
        return nil;
    return swizzled((id<MTLTexture>)t, d.swizzle);
}

// SPI (Core Animation): the same, one 2D image
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    (void)image;
    return [self newTextureWithDescriptor:d offset:offset bytesPerRow:row];
}

@end
