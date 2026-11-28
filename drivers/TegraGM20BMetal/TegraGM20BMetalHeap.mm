// SPDX-License-Identifier: MIT
// MTLHeaps as Metal has them (RenderBox's ResourceHeap: textures from heap newTextureWithDescriptor:, their room handed back
// by makeAliasable, occupancy read off usedSize, the heap made volatile when recycled). Every texture the driver keeps and
// every buffer is placed in the heap's arena (gm20b::Device::createArena), laid out as the device lays its kind out, so what
// the heap holds is its size, a job names it as one BO and a purged heap holds no pages. A texture the arena lays out none
// of (one with a stand-in, or of a shape it places none of) is the device's own, counted against the heap's size. A heap
// resource's purgeable state is its heap's.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalMultisample.h"
#import "TegraGM20BMetalDecoded.h"

#include "decoded.h"
#include "srgb_pair.h"
#include "xr.h"
#include "ycbcr.h"

namespace {

bool arenaKind(MTLStorageMode mode, bool &blockLinear)
{
    blockLinear = mode == MTLStorageModePrivate;
    return blockLinear || mode == MTLStorageModeShared;
}

// The arena's pages, back if it was purged, before a command buffer reaches it (a placement brings them back under
// the device's lock: arena.cpp)
bool resident(SwitchMetalHeap *h)
{
    if (!h || !h->_arena || gRM->raw->restoreArena(h->_arena - 1))
        return true;
    os_log_error(gm20bLog(), "SwitchMetal: a purged heap's pages were not made again (%llu bytes)",
                 (unsigned long long)h->_descriptor.size);
    return false;
}

bool matchingOptions(SwitchMetalHeap *h, MTLResourceOptions options)
{
    constexpr NSUInteger mask = MTLResourceStorageModeMask | MTLResourceCPUCacheModeMask;
    return (options & mask) == (h->_descriptor.resourceOptions & mask);
}

id<MTLBuffer> makeArenaBuffer(SwitchMetalHeap *h, NSUInteger length, MTLResourceOptions options,
                              std::optional<uint64_t> offset = {})
{
    if (!matchingOptions(h, options))
        return nil;
    SwitchMetalBuffer *b = [SwitchMetalBuffer new];
    b->_device = h->_device;
    b->_options = options;
    if (!gRM->raw->arenaBuffer(h->_arena - 1, length, b->_raw, offset))
        return nil;
    b->_heap = h;
    b->_raw.hostVisible = h->_descriptor.storageMode != MTLStorageModePrivate;
    if (b->_raw.hostVisible) {
        gm20b::HostAccess access;
        if (!gRM->raw->hostAccess(b->_raw, true, access) || !access.finish())
            return nil;
    }
    b->_aliasable = offset.has_value();
    return (id<MTLBuffer>)b;
}

// X32_Stencil8: a Depth32Float_Stencil8 texture's memory read through its stencil, as gm20bNewTexture makes one
// The descriptor a heap's texture of `d` is laid out by: X32_Stencil8's combined format; a decoded format's stored one
// (decoded.h), laid out as a render target as gm20b::shapes::heldShape lays it out; else `d`
MTLTextureDescriptor *heldDescriptor(MTLTextureDescriptor *d)
{
    const gm20b::DecodedFormat *decoded = gm20b::decodedFormat(uint32_t(d.pixelFormat));
    if (d.pixelFormat != MTLPixelFormatX32_Stencil8 && !decoded)
        return d;
    MTLTextureDescriptor *held = [d copy];
    held.pixelFormat = decoded ? MTLPixelFormat(decoded->stored) : MTLPixelFormatDepth32Float_Stencil8;
    if (decoded)
        held.usage = d.usage | MTLTextureUsageRenderTarget;
    return held;
}

id<MTLTexture> makeArenaTexture(SwitchMetalHeap *h, MTLTextureDescriptor *d, const gm20b::TextureAsk &ask,
                                std::optional<uint64_t> offset = {})
{
    if (!matchingOptions(h, d.resourceOptions))
        return nil;
    if (MTLTextureDescriptor *held = heldDescriptor(d); held != d) {
        id<MTLTexture> made = makeArenaTexture(h, held, ask, offset);
        if (!gm20b::decodedFormat(uint32_t(d.pixelFormat)))
            return [made newTextureViewWithPixelFormat:d.pixelFormat];
        if (made)
            ((SwitchMetalTexture *)made)->_descriptor = [d copy];
        return made && gm20bKeepDecodedBlocks((SwitchMetalTexture *)made) ? made : nil;
    }
    SwitchMetalTexture *t = [SwitchMetalTexture new];
    t->_device = h->_device;
    t->_descriptor = [d copy];
    t->_kind = SwitchMetalTexturePool;
    if (!gRM->raw->arenaTexture({h->_arena - 1, offset}, ask, t->_raw))
        return nil;
    if (ask.kind == gm20b::TextureAsk::Plain && !keepExtendedRange(d, t->_raw, false)) {
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: no memory or texture header for a heap's %lux%lu format %lu texture's "
                     "stand-in: nil",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
        gRM->raw->destroyTexture(t->_raw);
        return nil;
    }
    t->_gm20bOk = true;
    t->_heap = h;
    t->_aliasable = offset.has_value();
    return swizzled((id<MTLTexture>)t, d.swizzle);
}

}  // namespace

// The ask gm20bNewTexture (TegraGM20BMetalDevice.mm) makes `d` with, in its order of kinds; false: a texture the driver keeps
// none of, or one the arena's storage mode does not hold. An extended-range one is a one-level 2D texture's alone: the arena
// lays a stand-in out beside its codes for that one (Device::arenaTexture); an RG8Unorm_sRGB one none. `placed`: false for
// a size asked of one the device makes outside the arena, which every extended-range and RG8Unorm_sRGB shape it keeps is,
// with the stand-in a draw samples
static bool textureAsk(MTLTextureDescriptor *d, bool blockLinear, gm20b::TextureAsk &ask, bool placed)
{
    // a decoded format's shape is judged as asked, before it is laid out as its stored format's
    if (const gm20b::shapes::TextureShape s = textureShape(d);
        gm20b::decodedFormat(s.format) &&
        (gm20b::shapes::metalRefusal(s) || gm20b::shapes::identityRefusal(s, gm20b::answers::kThisKind)))
        return false;
    d = heldDescriptor(d);
    bool tiled;
    const bool standIn = gm20b::xrConversionOf(uint32_t(d.pixelFormat)) || gm20b::srgbPair(uint32_t(d.pixelFormat));
    if (!arenaKind(d.storageMode, tiled) || tiled != blockLinear ||
        (placed && gm20b::xrSampled(uint32_t(d.pixelFormat)) && !gm20bShape(d)) ||
        (placed && gm20b::srgbPair(uint32_t(d.pixelFormat))))
        return false;
    ask = {};
    ask.width = uint32_t(d.width);
    ask.height = uint32_t(d.height);
    ask.format = uint32_t(d.pixelFormat);
    gm20b::shapes::TextureShape plain = textureShape(d);
    plain.ownMemory = !placed;
    const SwitchMetalStoredSamples stored = storedMultisampleKind(d);
    const uint32_t array = arrayType(d);
    if (chainShape(d)) {
        ask.kind = gm20b::TextureAsk::Mips;
        ask.levels = uint32_t(d.mipmapLevelCount);
    } else if (const gm20b::shapes::TextureShape s = textureShape(d); gm20b::shapes::blockShape(s)) {
        ask.kind = gm20b::TextureAsk::Block;
        ask.levels = uint32_t(std::max<uint64_t>(s.levels, 1));
        ask.layers = gm20b::textureLayers(s.type, uint32_t(std::min<uint64_t>(s.array, 2048)));
        ask.depth = uint32_t(std::max<uint64_t>(s.depth, 1));
        ask.type = gm20b::shapes::headerType(s.type);
    } else if (lineShape(d)) {
        ask.kind = gm20b::TextureAsk::Line;
    } else if (stored != SwitchMetalStoredSamples::None) {
        ask.kind = stored == SwitchMetalStoredSamples::Zeta ? gm20b::TextureAsk::Zeta : gm20b::TextureAsk::Multisample;
        ask.samples = uint32_t(d.sampleCount);
        ask.sampled = (d.usage & MTLTextureUsageShaderRead) != 0;
    } else if (volumeShape(d)) {
        ask.kind = gm20b::TextureAsk::Volume;
        ask.depth = uint32_t(d.depth);
        ask.view2d = gm20b::shapes::volumeView2d(textureShape(d));
        ask.levels = uint32_t(std::max<NSUInteger>(d.mipmapLevelCount, 1));
    } else if (array) {
        const bool cube = array == gm20b::fx::kTexCube || array == gm20b::fx::kTexCubeArray;
        ask.kind = gm20b::TextureAsk::Array;
        ask.type = uint8_t(array);
        ask.layers = uint32_t(std::max<NSUInteger>(d.arrayLength, 1) * (cube ? 6 : 1));
        ask.levels = uint32_t(std::max<NSUInteger>(d.mipmapLevelCount, 1));
    } else if (const gm20b::shapes::TextureShape s = textureShape(d);
               zetaDescriptor(d) && gm20b::shapes::zetaShape(s)) {
        ask.kind = gm20b::TextureAsk::Zeta;
        ask.sampled = d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderRead);
        ask.type = gm20b::shapes::headerType(s.type);
        ask.layers = gm20b::textureLayers(s.type, uint32_t(std::min<uint64_t>(s.array, 2048)));
        ask.levels = uint32_t(std::max<uint64_t>(s.levels, 1));
    } else if (gm20b::shapes::gm20bShape(plain)) {
        ask.tiled = blockLinear || gm20b::blockCompressed(gm20b::formatInfo(ask.format));
    } else {
        return false;
    }
    ask.standIn = standIn && (d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderRead)) &&
                  (!placed || ask.kind == gm20b::TextureAsk::Plain);
    uint64_t bytes, align;
    return gm20b::Device::footprint(ask, bytes, align);
}

bool heapTextureAsk(MTLTextureDescriptor *d, bool blockLinear, gm20b::TextureAsk &ask)
{
    return textureAsk(d, blockLinear, ask, true);
}

// The alignment Metal's heap query answers, with 0 bytes, for a texture it makes none of (m2v texture/heap.rs
// no_texture_alignment, measured on an M4 Max)
static uint64_t noTextureAlignment(const gm20b::shapes::TextureShape &s)
{
    if (gm20b::isYcbcrFormat(s.format))
        return s.type == gm20b::kType2DArray ? 128 : 16;
    if (gm20b::twoPlaneXr(s.format))
        return 16;
    switch (s.type) {
    case gm20b::kType1D:
    case gm20b::kType2D:
    case gm20b::kType2DMultisample:
        return 128;
    case 9:  // MTLTextureTypeTextureBuffer
        return 16;
    default:
        return 16 << 10;
    }
}

void gm20bHeapHold(SwitchMetalHeap *heap, const gm20b::HeapMember &member)
{
    if (heap && heap->_members)
        heap->_members->add(member);
}

void gm20bHeapForget(SwitchMetalHeap *heap, uint64_t va)
{
    if (heap && heap->_members)
        heap->_members->remove(va);
}

@implementation SwitchMetalHeap

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
    return NSUInteger(_used.load(std::memory_order_relaxed) + (_arena ? gRM->raw->arenaUsed(_arena - 1) : 0));
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

// Volatile: its pages go back once the device is idle (TegraGM20BMetal.mm's release); Empty: now, if nothing is in flight. A later
// NonVolatile answers Empty when they went, its resources' contents then undefined, as Metal's are.
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const int asked = _purgeable.load(std::memory_order_relaxed);
    const auto answer = [&](bool purged) {
        return purged ? MTLPurgeableStateEmpty : asked ? MTLPurgeableState(asked) : MTLPurgeableStateNonVolatile;
    };
    if (state == MTLPurgeableStateKeepCurrent)
        return answer(_arena && gRM->raw->arenaPurged(_arena - 1));
    const bool keep = state == MTLPurgeableStateNonVolatile;
    _purgeable.store(keep ? MTLPurgeableStateNonVolatile : MTLPurgeableStateVolatile, std::memory_order_relaxed);
    if (!_arena)
        return answer(false);
    // whether the pages had gone and the new volatility in one step: no purge falls between the answer and the promise
    const MTLPurgeableState old = answer(gRM->raw->setArenaVolatile(_arena - 1, !keep));
    if (keep) {
        if (!resident(self) && gRM->raw->setArenaVolatileIfPurged(_arena - 1)) {
            int expected = MTLPurgeableStateNonVolatile;
            _purgeable.compare_exchange_strong(expected, MTLPurgeableStateVolatile, std::memory_order_acq_rel,
                                               std::memory_order_relaxed);
        }
    } else if (state == MTLPurgeableStateEmpty) {
        gRM->raw->purgeArena(_arena - 1, gRM->committed);
    }
    return old;
}

- (bool)gm20bTake:(uint64_t)bytes
{
    if (_used.fetch_add(bytes, std::memory_order_relaxed) + bytes + (_arena ? gRM->raw->arenaUsed(_arena - 1) : 0) <=
        _descriptor.size)
        return true;
    _used.fetch_sub(bytes, std::memory_order_relaxed);
    return false;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    if (_descriptor.type == MTLHeapTypePlacement)
        return nil;
    if (_arena)
        return makeArenaBuffer(self, length, options);
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapBufferSizeAndAlignWithLength:length options:options];
    if (![self gm20bTake:need.size])
        return nil;
    id b = [(id<MTLDevice>)_device newBufferWithLength:length options:options];
    if (!gm20bAsBuffer(b)) {
        _used.fetch_sub(need.size, std::memory_order_relaxed);
        return (id<MTLBuffer>)b;
    }
    SwitchMetalBuffer *made = (SwitchMetalBuffer *)b;
    made->_heap = self;
    made->_heapBytes = need.size;
    gm20bHeapHold(self, {made->_raw.va, made->_raw.length, made->_raw.hostVisible, made->_raw.gpuVolatile});
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
    gm20b::TextureAsk ask;
    if (_arena && arenaKind(_descriptor.storageMode, blockLinear) && heapTextureAsk(d, blockLinear, ask))
        return makeArenaTexture(self, d, ask);
    const MTLSizeAndAlign need = [(id<MTLDevice>)_device heapTextureSizeAndAlignWithDescriptor:d];
    if (![self gm20bTake:need.size])
        return nil;
    id t = [(id<MTLDevice>)_device newTextureWithDescriptor:d];
    if (!gm20bAsTexture(t)) {
        _used.fetch_sub(need.size, std::memory_order_relaxed);
        return (id<MTLTexture>)t;
    }
    SwitchMetalTexture *made = (SwitchMetalTexture *)t;
    made->_heap = self;
    made->_heapBytes = need.size;
    const gm20b::Texture::StorageRange storage = made->_raw.storageRange();
    gm20bHeapHold(self, {storage.bytes ? storage.va : made->_raw.va, storage.bytes ? storage.bytes : 1,
                         storage.hostVisible, storage.gpuVolatile});
    return (id<MTLTexture>)t;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    bool blockLinear;
    gm20b::TextureAsk ask;
    if (_descriptor.type != MTLHeapTypePlacement || !_arena || !arenaKind(_descriptor.storageMode, blockLinear) ||
        !heapTextureAsk(d, blockLinear, ask))
        return nil;
    return makeArenaTexture(self, d, ask, offset);
}

@end

@implementation SwitchMetalDevice (Heaps)

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    if (d.type != MTLHeapTypeAutomatic && d.type != MTLHeapTypePlacement)
        return nil;
    SwitchMetalHeap *h = [SwitchMetalHeap new];
    h->_device = self;
    h->_descriptor = [d copy];
    bool blockLinear;
    uint32_t arena;
    if ((d.type == MTLHeapTypeAutomatic || d.type == MTLHeapTypePlacement) && arenaKind(d.storageMode, blockLinear) &&
        gRM->raw->createArena(d.size, blockLinear, arena, d.type == MTLHeapTypePlacement))
        h->_arena = arena + 1;
    if (d.type == MTLHeapTypePlacement && !h->_arena)
        return nil;
    h->_members = std::make_shared<gm20b::HeapMembers>(h->_arena ? h->_arena - 1 : gm20b::HeapMembers::kNoArena,
                                                       d.storageMode != MTLStorageModePrivate);
    return (id<MTLHeap>)h;
}

// A heap's size of a texture: the footprint of the one the device makes (Managed storage is Shared on Apple silicon); 0
// bytes at Metal's alignment for it where Metal makes none (m2v texture/heap.rs no_texture_alignment, measured); 0 for a
// Memoryless one, which no heap holds (Metal's validation stops the query)
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
    if (d.storageMode == MTLStorageModeMemoryless) {
        SWITCH_REFUSE("gm20b", "heap.memoryless",
                      "a heap's size of a Memoryless %lux%lu texture of format %lu: no heap holds one: 0 bytes",
                      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
        return {0, 0};
    }
    gm20b::shapes::TextureShape s = textureShape(d);
    s.ownMemory = true;
    if (gm20b::shapes::metalRefusal(s) || gm20b::shapes::identityRefusal(s, gm20b::answers::kThisKind) ||
        gm20b::shapes::hardwareRefusal(s))
        return {0, NSUInteger(noTextureAlignment(s))};
    // a texture buffer: its texels' bytes at 16 (m2v texture/heap.rs; an RGB10A8_2P_XR10 one's alpha bytes past its words,
    // xr.h texelBufferBytes: an M4 Max), the buffer the device makes it in
    if (const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
        f && d.textureType == MTLTextureTypeTextureBuffer)
        return {NSUInteger((gm20b::texelBufferBytes(uint32_t(d.pixelFormat), d.width, f->bpp) + 15) & ~uint64_t(15)),
                16};
    MTLTextureDescriptor *asked = d;
    if (uint32_t(d.storageMode) == gm20b::shapes::kManaged) {
        asked = [d copy];
        asked.storageMode = MTLStorageModeShared;
    }
    bool blockLinear;
    gm20b::TextureAsk ask;
    uint64_t bytes, align;
    if (arenaKind(asked.storageMode, blockLinear) && textureAsk(asked, blockLinear, ask, false) &&
        gm20b::Device::footprint(ask, bytes, align))
        return {NSUInteger(bytes), NSUInteger(align)};
    // every shape the device makes has a footprint (textureAsk takes route()'s creators in its order): one with none is
    // too large to lay out, which the device makes none of either
    os_log_error(gm20bLog(),
                 "SwitchMetal: %{public}s: a heap's size of a %lux%lux%lu format %lu texture the device lays out none "
                 "of: 0 bytes",
                 getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth,
                 (unsigned long)d.pixelFormat);
    return {0, NSUInteger(noTextureAlignment(s))};
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    (void)options;
    return {(std::max<NSUInteger>(length, 16) + 255) & ~NSUInteger(255), 256};
}

@end

void restoreArenas(SwitchMetalCommandBuffer *cb)
{
    auto restoreObject = [](id o) {
        SwitchMetalHeap *h = nil;
        if ([o isKindOfClass:[SwitchMetalTexture class]]) {
            __unsafe_unretained SwitchMetalTexture *owner =
                ((SwitchMetalTexture *)o)->_levelsOwner ?: (SwitchMetalTexture *)o;
            h = owner->_heap ?: (owner->_buffer ? owner->_buffer->_heap : nil);
        } else if ([o isKindOfClass:[SwitchMetalBuffer class]])
            h = ((SwitchMetalBuffer *)o)->_heap;
        else if ([o isKindOfClass:[SwitchMetalHeap class]])
            h = (SwitchMetalHeap *)o;
        resident(h);
    };
    for (id o : cb->_owners)
        restoreObject(o);
    for (const auto &snapshot : cb->_residencySnapshots)
        for (id o in snapshot->committed)
            restoreObject(o);
}
