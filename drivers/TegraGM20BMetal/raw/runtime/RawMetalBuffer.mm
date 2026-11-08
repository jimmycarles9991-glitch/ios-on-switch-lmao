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
    if (gRM)
        gRM->raw->destroyBuffer(_raw);
    if (_heap)
        _heap->_used -= length;
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
    const size_t page = size_t(getpagesize());
    const MTLResourceOptions options = (_options & ~MTLResourceStorageModeMask) | MTLResourceStorageModeShared;
    raw::Alias a;
    return twinPublish(self, _twin,
                       gRM->raw->alias(_raw, a)
                           ? rawAliasBuffer(a, _raw.cpu, (NSUInteger(_raw.length) + page - 1) & ~(page - 1), options)
                           : nil);
}

// A CPU read after a GPU write: the GPU's bytes, not lines the CPU cached before (clean+invalidate keeps any CPU write)
- (void)invalidateForCpu
{
    if (!_gpuDirty.exchange(false) || _raw.wc)
        return;  // (write-combined: the CPU reads memory itself)
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(_raw.cpu) & ~uintptr_t(63); a < uintptr_t(_raw.cpu) + _raw.length; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

- (void *)contents
{
    [self invalidateForCpu];
    return _raw.cpu;
}

- (NSUInteger)length
{
    return NSUInteger(_raw.length);
}

- (NSUInteger)heapOffset
{
    return 0;  // a heap's buffers are allocations of their own (RMHeap)
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

// The pages are never given back while the buffer lives: Volatile and Empty keep the contents (a later NonVolatile answers what
// was asked before, never Empty), which is what a caller that refills on Empty needs.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
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
{}

- (BOOL)isAliasable
{
    return NO;
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
    return (id<MTLTexture>)t;
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

@implementation RMHeap

- (id)rmTwin
{
    return nil;  // the facade's heap is its own: its buffers and textures are ordinary facade resources
}

- (NSUInteger)size
{
    return _descriptor.size;
}

- (NSUInteger)usedSize
{
    return NSUInteger(_used.load());
}

- (NSUInteger)currentAllocatedSize
{
    return _descriptor.size;
}

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    (void)alignment;
    const uint64_t used = _used.load();
    return used >= _descriptor.size ? 0 : NSUInteger(_descriptor.size - used);
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

- (MTLHeapType)type
{
    return _descriptor.type;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    (void)state;
    return MTLPurgeableStateNonVolatile;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if ([b isKindOfClass:[RMBuffer class]]) {
        ((RMBuffer *)b)->_heap = self;
        _used += length;
    }
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    (void)offset;
    return [self newBufferWithLength:length options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [(id<MTLDevice>)_device newTextureWithDescriptor:d];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    (void)offset;
    return [self newTextureWithDescriptor:d];
}

@end
