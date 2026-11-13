// SPDX-License-Identifier: MIT
// MTLHeaps as Metal has them (RenderBox's ResourceHeap: textures from heap newTextureWithDescriptor:, their room handed back
// by makeAliasable, occupancy read off usedSize, the heap made volatile when recycled). Every texture the raw path keeps and
// every buffer is placed in the heap's arena (raw::Device::createArena), laid out as the device lays its kind out, so what
// the heap holds is its size, a job names it as one BO and a purged heap holds no pages. A texture the raw path keeps none
// of is m2v's, counted against the heap's size. A heap resource's purgeable state is its heap's.
#import "RawMetalInternal.h"
#import "RawMetalMultisample.h"

#include "raw_xr.h"

namespace {

bool arenaKind(MTLStorageMode mode, bool &blockLinear)
{
    blockLinear = mode == MTLStorageModePrivate;
    return blockLinear || mode == MTLStorageModeShared;
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

bool matchingOptions(RMHeap *h, MTLResourceOptions options)
{
    constexpr NSUInteger mask = MTLResourceStorageModeMask | MTLResourceCPUCacheModeMask;
    return (options & mask) == (h->_descriptor.resourceOptions & mask);
}

id<MTLBuffer> makeArenaBuffer(RMHeap *h, NSUInteger length, MTLResourceOptions options,
                              std::optional<uint64_t> offset = {})
{
    if (!matchingOptions(h, options) || !resident(h))
        return nil;
    RMBuffer *b = [RMBuffer new];
    b->_device = h->_device;
    b->_options = options;
    if (!gRM->raw->arenaBuffer(h->_arena - 1, length, b->_raw, offset))
        return nil;
    b->_heap = h;
    b->_raw.hostVisible = h->_descriptor.storageMode != MTLStorageModePrivate;
    if (b->_raw.hostVisible) {
        raw::HostAccess access;
        if (!gRM->raw->hostAccess(b->_raw, true, access) || !access.finish())
            return nil;
    }
    b->_aliasable = offset.has_value();
    return (id<MTLBuffer>)b;
}

id<MTLTexture> makeArenaTexture(RMHeap *h, MTLTextureDescriptor *d, const raw::TextureAsk &ask,
                                std::optional<uint64_t> offset = {})
{
    if (!matchingOptions(h, d.resourceOptions) || !resident(h))
        return nil;
    RMTexture *t = [RMTexture new];
    t->_device = h->_device;
    t->_descriptor = [d copy];
    t->_kind = RMTexturePool;
    if (!gRM->raw->arenaTexture({h->_arena - 1, offset}, ask, t->_raw))
        return nil;
    if (ask.kind == raw::TextureAsk::Plain)
        keepExtendedRange(d, t->_raw, false);
    t->_rawOk = true;
    t->_heap = h;
    t->_aliasable = offset.has_value();
    return swizzled((id<MTLTexture>)t, d.swizzle);
}

}  // namespace

// The ask rmNewTexture (RawMetalDevice.mm) makes `d` with, in its order of kinds; false: a texture the raw path keeps none of
// (m2v's alone, which no job of the raw path names), or one the arena's storage mode does not hold
bool heapTextureAsk(MTLTextureDescriptor *d, bool blockLinear, raw::TextureAsk &ask)
{
    bool tiled;
    if (!arenaKind(d.storageMode, tiled) || tiled != blockLinear)
        return false;
    ask = {};
    ask.width = uint32_t(d.width);
    ask.height = uint32_t(d.height);
    ask.format = uint32_t(d.pixelFormat);
    const RMStoredSamples stored = storedMultisampleKind(d);
    const uint32_t array = arrayType(d);
    if (chainShape(d)) {
        ask.kind = raw::TextureAsk::Mips;
        ask.levels = uint32_t(d.mipmapLevelCount);
    } else if (lineShape(d)) {
        ask.kind = raw::TextureAsk::Line;
    } else if (stored != RMStoredSamples::None) {
        ask.kind = stored == RMStoredSamples::Zeta ? raw::TextureAsk::Zeta : raw::TextureAsk::Multisample;
        ask.samples = uint32_t(d.sampleCount);
        ask.sampled = (d.usage & MTLTextureUsageShaderRead) != 0;
    } else if (volumeShape(d)) {
        ask.kind = raw::TextureAsk::Volume;
        ask.depth = uint32_t(d.depth);
        ask.view2d = volumeRendered(d);
        ask.levels = uint32_t(std::max<NSUInteger>(d.mipmapLevelCount, 1));
    } else if (array) {
        const bool cube = array == raw::fx::kTexCube || array == raw::fx::kTexCubeArray;
        ask.kind = raw::TextureAsk::Array;
        ask.type = uint8_t(array);
        ask.layers = uint32_t(std::max<NSUInteger>(d.arrayLength, 1) * (cube ? 6 : 1));
        ask.levels = uint32_t(std::max<NSUInteger>(d.mipmapLevelCount, 1));
    } else if (zetaDescriptor(d)) {
        ask.kind = raw::TextureAsk::Zeta;
        ask.sampled = (d.usage & MTLTextureUsageShaderRead) != 0;
    } else if (rawShape(d)) {
        ask.tiled = blockLinear;
        ask.standIn = raw::xrForm(ask.format) != raw::XrForm::None &&
                      (d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderRead));
    } else {
        return false;
    }
    uint64_t bytes, align;
    return raw::Device::footprint(ask, bytes, align);
}

bool heapResident(RMHeap *h)
{
    return resident(h);
}

void rmHeapHold(RMHeap *heap, const raw::HeapMember &member)
{
    if (heap && heap->_members)
        heap->_members->add(member);
}

void rmHeapForget(RMHeap *heap, uint64_t va)
{
    if (heap && heap->_members)
        heap->_members->remove(va);
}

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
    const int asked = _purgeable.load();
    const auto answer = [&](bool purged) {
        return purged ? MTLPurgeableStateEmpty : asked ? MTLPurgeableState(asked) : MTLPurgeableStateNonVolatile;
    };
    if (state == MTLPurgeableStateKeepCurrent)
        return answer(_arena && gRM->raw->arenaPurged(_arena - 1));
    const bool keep = state == MTLPurgeableStateNonVolatile;
    _purgeable = keep ? MTLPurgeableStateNonVolatile : MTLPurgeableStateVolatile;
    if (!_arena)
        return answer(false);
    // whether the pages had gone and the new volatility in one step: no purge falls between the answer and the promise
    const MTLPurgeableState old = answer(gRM->raw->setArenaVolatile(_arena - 1, !keep));
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
    if (_descriptor.type == MTLHeapTypePlacement)
        return nil;
    if (_arena)
        return makeArenaBuffer(self, length, options);
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapBufferSizeAndAlignWithLength:length options:options];
    if (![self rmTake:need.size])
        return nil;
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if (![b isKindOfClass:[RMBuffer class]]) {
        _used -= need.size;
        return (id<MTLBuffer>)b;
    }
    RMBuffer *made = (RMBuffer *)b;
    made->_heap = self;
    made->_heapBytes = need.size;
    rmHeapHold(self, {made->_raw.va, made->_raw.length, made->_raw.hostVisible});
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    if (_descriptor.type != MTLHeapTypePlacement || !_arena)
        return nil;
    return makeArenaBuffer(self, length, options, offset);
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    if (_descriptor.type == MTLHeapTypePlacement)
        return nil;
    bool blockLinear;
    raw::TextureAsk ask;
    if (_arena && arenaKind(_descriptor.storageMode, blockLinear) && heapTextureAsk(d, blockLinear, ask))
        return makeArenaTexture(self, d, ask);
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapTextureSizeAndAlignWithDescriptor:d];
    if (![self rmTake:need.size])
        return nil;
    id t = [(id<MTLDevice>)_device newTextureWithDescriptor:d];
    if (![t isKindOfClass:[RMTexture class]]) {
        _used -= need.size;
        return (id<MTLTexture>)t;
    }
    RMTexture *made = (RMTexture *)t;
    made->_heap = self;
    made->_heapBytes = need.size;
    const raw::Texture::StorageRange storage = made->_raw.storageRange();
    rmHeapHold(self,
               {storage.bytes ? storage.va : made->_raw.va, storage.bytes ? storage.bytes : 1, storage.hostVisible});
    return (id<MTLTexture>)t;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    bool blockLinear;
    raw::TextureAsk ask;
    if (_descriptor.type != MTLHeapTypePlacement || !_arena || !arenaKind(_descriptor.storageMode, blockLinear) ||
        !heapTextureAsk(d, blockLinear, ask))
        return nil;
    return makeArenaTexture(self, d, ask, offset);
}

@end

@implementation RMDevice (Heaps)

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    if (d.type != MTLHeapTypeAutomatic && d.type != MTLHeapTypePlacement)
        return nil;
    RMHeap *h = [RMHeap new];
    h->_device = self;
    h->_descriptor = [d copy];
    bool blockLinear;
    uint32_t arena;
    if ((d.type == MTLHeapTypeAutomatic || d.type == MTLHeapTypePlacement) && arenaKind(d.storageMode, blockLinear) &&
        gRM->raw->createArena(d.size, blockLinear, arena, d.type == MTLHeapTypePlacement))
        h->_arena = arena + 1;
    if (d.type == MTLHeapTypePlacement && !h->_arena)
        return nil;
    h->_members = std::make_shared<raw::HeapMembers>(h->_arena ? h->_arena - 1 : raw::HeapMembers::kNoArena,
                                                     d.storageMode != MTLStorageModePrivate);
    return (id<MTLHeap>)h;
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
    bool blockLinear;
    raw::TextureAsk ask;
    uint64_t bytes, align;
    if (arenaKind(d.storageMode, blockLinear) && heapTextureAsk(d, blockLinear, ask) &&
        raw::Device::footprint(ask, bytes, align))
        return {NSUInteger(bytes), NSUInteger(align)};
    return [RM_M2V("a heap texture's size the raw path does not place") heapTextureSizeAndAlignWithDescriptor:d];
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
