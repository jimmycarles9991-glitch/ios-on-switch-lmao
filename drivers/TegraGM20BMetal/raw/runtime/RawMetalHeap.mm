// SPDX-License-Identifier: MIT
// MTLHeaps as Metal has them (RenderBox's ResourceHeap: textures from heap newTextureWithDescriptor:, their room handed back
// by makeAliasable, occupancy read off usedSize, the heap made volatile when recycled). A private heap's 2D textures are placed
// block-linear and a shared heap's 2D textures and buffers pitch-linear in its arena (raw::Device::createArena), so what the
// heap holds is its size, and a purged heap holds no pages. Any other resource is made as the device makes it and counted
// against the heap's size. A heap resource's purgeable state is its heap's.
#import "RawMetalInternal.h"

#include "raw_xr.h"

namespace {

bool arenaKind(MTLStorageMode mode, bool &blockLinear)
{
    blockLinear = mode == MTLStorageModePrivate;
    return blockLinear || mode == MTLStorageModeShared;
}

// A texture an arena of `blockLinear` places: one 2D level of one sample, the raw path's own format, no extended range
bool placed(MTLTextureDescriptor *d, bool blockLinear, uint64_t &bytes, uint64_t &align)
{
    bool tiled;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           arenaKind(d.storageMode, tiled) && tiled == blockLinear && rawShape(d) && !zetaDescriptor(d) &&
           raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None &&
           raw::Device::arenaFootprint(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), blockLinear,
                                       bytes, align);
}

// The arena's pages, back if it was purged: before anything is placed in it or a command buffer reaches it
bool resident(RMHeap *h)
{
    if (!h || !h->_arena || gRM->raw->restoreArena(h->_arena - 1))
        return true;
    os_log_error(rawLog(), "raw: a purged heap's pages were not made again (%llu bytes)",
                 (unsigned long long)h->_descriptor.size);
    return false;
}

}  // namespace

@implementation RMHeap

- (id)rmTwin
{
    return nil;  // the facade's heap is its own: its buffers and textures are ordinary facade resources
}

- (void)dealloc
{
    if (_arena && gRM)
        gRM->raw->destroyArena(_arena - 1);
}

- (NSUInteger)size
{
    return _descriptor.size;
}

// MTLAllocation's: the heap's whole size, what a residency set holding it sums
- (NSUInteger)allocatedSize
{
    return _descriptor.size;
}

- (NSUInteger)usedSize
{
    return NSUInteger(_used.load() + (_arena ? gRM->raw->arenaUsed(_arena - 1) : 0));
}

- (NSUInteger)currentAllocatedSize
{
    return _descriptor.size;
}

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    const uint64_t used = [self usedSize], left = used >= _descriptor.size ? 0 : _descriptor.size - used;
    return NSUInteger(_arena ? std::min<uint64_t>(left, gRM->raw->arenaLargestFree(_arena - 1, alignment)) : left);
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

// Volatile: its pages go back once the device is idle (RawMetal.mm's release); Empty: now, if nothing is in flight. A later
// NonVolatile answers Empty when they went, its resources' contents then undefined, as Metal's are.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const bool purged = _arena && gRM->raw->arenaPurged(_arena - 1);
    const int asked = _purgeable.load();
    const MTLPurgeableState old = purged  ? MTLPurgeableStateEmpty
                                  : asked ? MTLPurgeableState(asked)
                                          : MTLPurgeableStateNonVolatile;
    if (state == MTLPurgeableStateKeepCurrent)
        return old;
    const bool keep = state == MTLPurgeableStateNonVolatile;
    _purgeable = keep ? MTLPurgeableStateNonVolatile : MTLPurgeableStateVolatile;
    if (!_arena)
        return old;
    gRM->raw->setArenaVolatile(_arena - 1, !keep);
    if (keep)
        resident(self);
    else if (state == MTLPurgeableStateEmpty)
        gRM->raw->purgeArena(_arena - 1, gRM->committed);
    return old;
}

- (bool)rmTake:(uint64_t)bytes
{
    if (_used.fetch_add(bytes) + bytes + (_arena ? gRM->raw->arenaUsed(_arena - 1) : 0) <= _descriptor.size)
        return true;
    _used -= bytes;
    return false;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    bool blockLinear;
    if (_arena && arenaKind(_descriptor.storageMode, blockLinear) && !blockLinear) {
        if (!resident(self))
            return nil;
        RMBuffer *b = [RMBuffer new];
        b->_device = _device;
        b->_options = options;
        if (!gRM->raw->arenaBuffer(_arena - 1, length, b->_raw))
            return nil;  // the heap is full: Metal answers nil
        b->_heap = self;
        return (id<MTLBuffer>)b;
    }
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapBufferSizeAndAlignWithLength:length options:options];
    if (![self rmTake:need.size])
        return nil;
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if (![b isKindOfClass:[RMBuffer class]]) {
        _used -= need.size;
        return (id<MTLBuffer>)b;
    }
    ((RMBuffer *)b)->_heap = self;
    ((RMBuffer *)b)->_heapBytes = need.size;
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    (void)offset;
    return [self newBufferWithLength:length options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    bool blockLinear;
    uint64_t bytes, align;
    if (_arena && arenaKind(_descriptor.storageMode, blockLinear) && placed(d, blockLinear, bytes, align)) {
        if (!resident(self))
            return nil;
        RMTexture *t = [RMTexture new];
        t->_device = _device;
        t->_descriptor = [d copy];
        t->_kind = RMTexturePool;
        if (!gRM->raw->arenaTexture(_arena - 1, uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                    t->_raw))
            return nil;
        t->_rawOk = true;
        t->_heap = self;
        return swizzled((id<MTLTexture>)t, d.swizzle);
    }
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapTextureSizeAndAlignWithDescriptor:d];
    if (![self rmTake:need.size])
        return nil;
    id t = [(id<MTLDevice>)_device newTextureWithDescriptor:d];
    if (![t isKindOfClass:[RMTexture class]]) {
        _used -= need.size;
        return (id<MTLTexture>)t;
    }
    ((RMTexture *)t)->_heap = self;
    ((RMTexture *)t)->_heapBytes = need.size;
    return (id<MTLTexture>)t;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    (void)offset;
    return [self newTextureWithDescriptor:d];
}

@end

@implementation RMDevice (Heaps)

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    RMHeap *h = [RMHeap new];
    h->_device = self;
    h->_descriptor = [d copy];
    bool blockLinear;
    uint32_t arena;
    if (d.type == MTLHeapTypeAutomatic && arenaKind(d.storageMode, blockLinear) &&
        gRM->raw->createArena(d.size, blockLinear, arena))
        h->_arena = arena + 1;
    return (id<MTLHeap>)h;
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
    bool blockLinear;
    uint64_t bytes, align;
    if (arenaKind(d.storageMode, blockLinear) && placed(d, blockLinear, bytes, align))
        return {NSUInteger(bytes), NSUInteger(align)};
    return [gRM->fallback heapTextureSizeAndAlignWithDescriptor:d];
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    (void)options;
    return {(std::max<NSUInteger>(length, 16) + 255) & ~NSUInteger(255), 256};
}

@end

void restoreArenas(RMCommandBuffer *cb)
{
    for (id o : cb->_owners) {
        RMHeap *h = nil;
        if ([o isKindOfClass:[RMTexture class]])
            h = ((RMTexture *)o)->_heap ?: (((RMTexture *)o)->_parent ? ((RMTexture *)o)->_parent->_heap : nil);
        else if ([o isKindOfClass:[RMBuffer class]])
            h = ((RMBuffer *)o)->_heap;
        else if ([o isKindOfClass:[RMHeap class]])
            h = (RMHeap *)o;
        resident(h);
    }
}
