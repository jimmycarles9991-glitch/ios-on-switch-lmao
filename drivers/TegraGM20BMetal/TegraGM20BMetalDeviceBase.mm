// SPDX-License-Identifier: MIT
// What the device class answers as an _MTLDevice of its own: the four calls Metal's registration makes, the identity and
// memory queries, a residency set, the library forms the base class sends back to the device (to the facade's own methods:
// TegraGM20BMetalCompiledLibraries.mm), and a selector it has no method for.
#import "TegraGM20BMetalInternal.h"
#include "limits_fill.h"

// The base class's limits table (LimitsTable.h): _MTLDevice's own accessors read it, and frameworks read it past them
// (PhotosUICore's 2D texture size, CoreRE's no-copy alignment); each field the device answers with a method of its own
// is written, as the frontend's fillLimits writes m2v's. Every field m2v's device answered the device answers too
// (harness/selcover's capability check). The host's _MTLDevice has a layout of its own.
void fillDeviceLimits(id device)
{
#if TARGET_OS_IPHONE
    static const SEL limits = NSSelectorFromString(@"limits");
    Class owner = class_getSuperclass(object_getClass(device));
    if (![owner instancesRespondToSelector:limits])
        return;
    auto *table = static_cast<uint8_t *>(((void *(*)(id, SEL))objc_msgSend)(device, limits));
    if (!table)
        return;
    fillRawLimits(device, owner, table, [](const LimitField &field, LimitFill how) {
        if (how != LimitFill::Filled)
            SWITCH_REFUSE_DETAIL("gm20b", "limits.layout", field.offset,
                                 "limits: %s not written at 0x%x (_MTLDevice does not read it there)", field.selector,
                                 field.offset);
    });
#else
    (void)device;
#endif
}

// The facade's own Core Image filter-function linking (TegraGM20BMetalCompiledLibraries.mm)
@protocol SwitchMetalFilterLibraries
- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error;
@end

API_AVAILABLE(ios(18.0), macos(15.0))
@interface SwitchMetalResidencySet : SwitchMetalObject <MTLResidencySet>
- (instancetype)initWithDescriptor:(MTLResidencySetDescriptor *)descriptor device:(id)device;
- (std::shared_ptr<const SwitchMetalResidencySnapshot>)gm20bSnapshot;
@end

@interface SwitchMetalDevice (Base)
@end
@implementation SwitchMetalDevice (Base)

// Metal's registration (_MTLAddDevice) runs these four on every device it registers; the target's own versions fill the
// base class's tables from an Apple GPU's hardware profile, which would claim its capabilities here
- (void)initLimits
{
    fillDeviceLimits(self);
}

- (void)initFeatureQueries
{}

- (void)initGPUFamilySupport
{}

- (void)initWorkarounds
{}

- (uint64_t)registryID
{
    return gRM->registryID;
}

// The GPU memory this device's objects hold
- (NSUInteger)currentAllocatedSize
{
    return NSUInteger(gRM->raw->allocatedBytes());
}

// Every allocation is resident from its creation to its release (one GPU address space, no paging): a residency set keeps
// its allocations for its client and asks nothing of the GPU, as m2v's does
- (id<MTLResidencySet>)newResidencySetWithDescriptor:(MTLResidencySetDescriptor *)descriptor
                                               error:(NSError **)error API_AVAILABLE(ios(18.0), macos(15.0))
{
    if (error)
        *error = nil;
    return [[SwitchMetalResidencySet alloc] initWithDescriptor:descriptor device:self];
}

// _MTLDevice sends Core Image's legacy DAG and a kernel's constants reflection here
- (id<MTLLibrary>)newLibraryWithImageFilterFunctionsSPI:(NSArray *)functions
                                imageFilterFunctionInfo:(const void *)info
                                                  error:(NSError **)error
{
    return [(id<SwitchMetalFilterLibraries>)self newLibraryWithCIFilters:functions
                                                 imageFilterFunctionInfo:info
                                                                   error:error];
}

// Core Image's stitched DAGs (CreateStitchedFunctionFromDescriptor), which the base class would send to the
// destinationBinaryArchive: form
- (id<MTLLibrary>)newLibraryWithDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                          destinationBinaryArchive:(id)archive
                                             error:(NSError **)error
{
    (void)archive;
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                       completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    id<MTLDevice> device = (id<MTLDevice>)self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithStitchedDescriptor:d error:&error];
        if (handler)
            handler(library, error);
    });
}

// Any other selector a Metal protocol declares is answered zero; anything else is not recognized
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (Method m = class_getInstanceMethod(object_getClass(self), selector))
        return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)];
    if (const char *types = metalTypes(selector))
        return [NSMethodSignature signatureWithObjCTypes:types];
    return nil;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    answerZero(self, invocation);
}

- (void)doesNotRecognizeSelector:(SEL)selector
{
    SWITCH_REFUSE_DETAIL("gm20b", "device.selector", uintptr_t(selector), "-[MTLDevice %s]: no method",
                         sel_getName(selector));
    const IMP base = class_getMethodImplementation(class_getSuperclass(object_getClass(self)), _cmd);
    reinterpret_cast<void (*)(id, SEL, SEL)>(base)(self, _cmd, selector);
}

@end

@implementation SwitchMetalResidencySet {
    gm20b::UnfairMutex _lock;
    NSMutableArray<id<MTLAllocation>> *_pending;
    NSArray<id<MTLAllocation>> *_committed;
    std::shared_ptr<const SwitchMetalResidencySnapshot> _snapshot;
}

@dynamic device, label;

- (instancetype)initWithDescriptor:(MTLResidencySetDescriptor *)descriptor device:(id)device
{
    if ((self = [super init])) {
        _device = device;
        self.label = descriptor.label;
        _pending = [NSMutableArray arrayWithCapacity:descriptor.initialCapacity];
        _committed = @[];
    }
    return self;
}

- (uint64_t)allocatedSize
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    uint64_t bytes = 0;
    for (id<MTLAllocation> allocation in _committed)
        bytes += allocation.allocatedSize;
    return bytes;
}

- (void)addAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    if (allocation && ![_pending containsObject:allocation])
        [_pending addObject:allocation];
}

- (void)addAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    if (!count)
        return;
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    for (NSUInteger i = 0; i < count; i++) {
        id<MTLAllocation> allocation = allocations[i];
        if (allocation && ![_pending containsObject:allocation])
            [_pending addObject:allocation];
    }
}

- (void)removeAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    [_pending removeObject:allocation];
}

- (void)removeAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    if (!count)
        return;
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    for (NSUInteger i = 0; i < count; i++)
        [_pending removeObject:allocations[i]];
}

- (void)removeAllAllocations
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    [_pending removeAllObjects];
}

- (BOOL)containsAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    return [_pending containsObject:allocation];
}

- (NSArray<id<MTLAllocation>> *)allAllocations
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    return [_pending copy];
}

- (NSUInteger)allocationCount
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    return _pending.count;
}

- (void)commit
{
    auto snapshot = std::make_shared<SwitchMetalResidencySnapshot>();
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    _committed = [_pending copy];
    snapshot->committed = _committed;
    for (id allocation in _committed) {
        if (SwitchMetalBuffer *b = gm20bAsBuffer(allocation)) {
            const MTLStorageMode mode =
                MTLStorageMode((b->_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
            if (mode != MTLStorageModePrivate && mode != MTLStorageModeMemoryless)
                snapshot->hostVisible.push_back({allocation, SwitchMetalResidencyEntry::Kind::Buffer, true});
        } else if (SwitchMetalTexture *t = gm20bAsTexture(allocation)) {
            const MTLStorageMode mode = t->_descriptor ? t->_descriptor.storageMode : MTLStorageModeShared;
            if (mode == MTLStorageModePrivate || mode == MTLStorageModeMemoryless)
                continue;
            const MTLTextureUsage usage = t->_descriptor ? t->_descriptor.usage : MTLTextureUsageUnknown;
            if (usage != MTLTextureUsageUnknown && !(usage & (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)))
                continue;
            __unsafe_unretained SwitchMetalTexture *root = (t->_levelsOwner && !t->_ownForm) ? t->_levelsOwner : t;
            const bool surface = root->_surface != nil;
            const bool write = surface ? (usage & MTLTextureUsageShaderWrite) != 0
                                       : (usage == MTLTextureUsageUnknown || (usage & MTLTextureUsageShaderWrite) != 0);
            snapshot->hostVisible.push_back({allocation, SwitchMetalResidencyEntry::Kind::Texture, write});
        } else if (SwitchMetalHeap *h = gm20bAsHeap(allocation)) {
            const MTLStorageMode mode = h->_descriptor ? h->_descriptor.storageMode : MTLStorageModeShared;
            if (mode != MTLStorageModePrivate && mode != MTLStorageModeMemoryless)
                snapshot->hostVisible.push_back({allocation, SwitchMetalResidencyEntry::Kind::Heap, false});
        }
    }
    _snapshot = std::move(snapshot);
}

- (std::shared_ptr<const SwitchMetalResidencySnapshot>)gm20bSnapshot
{
    std::lock_guard<gm20b::UnfairMutex> hold(_lock);
    return _snapshot;
}

- (void)requestResidency
{}

- (void)endResidency
{}

@end

void gm20bQueueAddResidencySet(SwitchMetalQueue *queue, id set)
{
    if (@available(iOS 18.0, macOS 15.0, *)) {
        if (![set isKindOfClass:[SwitchMetalResidencySet class]])
            return;
    } else {
        return;
    }
    std::lock_guard<gm20b::UnfairMutex> hold(queue->_residencyLock);
    if (!queue->_residencySets)
        queue->_residencySets = [NSMutableArray array];
    if (![queue->_residencySets containsObject:set]) {
        [queue->_residencySets addObject:set];
        queue->_residencySetCount.store(uint32_t(queue->_residencySets.count), std::memory_order_release);
    }
}

void gm20bQueueRemoveResidencySet(SwitchMetalQueue *queue, id set)
{
    if (!set)
        return;
    std::lock_guard<gm20b::UnfairMutex> hold(queue->_residencyLock);
    [queue->_residencySets removeObject:set];
    queue->_residencySetCount.store(uint32_t(queue->_residencySets.count), std::memory_order_release);
}

void gm20bUseResidencySet(SwitchMetalCommandBuffer *cb, id set)
{
    if (!cb)
        return;
    if (@available(iOS 18.0, macOS 15.0, *)) {
        if (![set isKindOfClass:[SwitchMetalResidencySet class]])
            return;
    } else {
        return;
    }
    SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.residencySetUses++);
    if (!cb->_residencySets)
        cb->_residencySets = [NSMutableArray array];
    if (![cb->_residencySets containsObject:set])
        [cb->_residencySets addObject:set];
}

void gm20bSnapshotResidencySets(SwitchMetalCommandBuffer *cb)
{
    SwitchMetalQueue *queue = cb->_queue;
    const bool hasQueueSets = queue && queue->_residencySetCount.load(std::memory_order_acquire) != 0;
    if (!cb->_residencySets && !hasQueueSets)
        return;
    if (@available(iOS 18.0, macOS 15.0, *)) {
        NSArray *queued = nil;
        if (hasQueueSets) {
            std::lock_guard<gm20b::UnfairMutex> hold(queue->_residencyLock);
            queued = [queue->_residencySets copy];
        }
        auto take = [&](SwitchMetalResidencySet *set) {
            if (auto snap = [set gm20bSnapshot]; snap && snap->committed.count)
                cb->_residencySnapshots.push_back(std::move(snap));
        };
        for (SwitchMetalResidencySet *set in cb->_residencySets)
            take(set);
        for (SwitchMetalResidencySet *set in queued)
            if (!cb->_residencySets || ![cb->_residencySets containsObject:set])
                take(set);
    }
    cb->_residencySets = nil;
}

static void nameResident(gm20b::CommandBuffer *rc, const SwitchMetalResidencyEntry &entry)
{
    switch (entry.kind) {
    case SwitchMetalResidencyEntry::Kind::Buffer: {
        SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.residencyNamed++);
        SwitchMetalBuffer *b = (SwitchMetalBuffer *)entry.allocation;
        if (b->_raw.hostVisible && !b->_raw.wc)
            rc->clean(b->_raw.cpu, b->_raw.length, true);
        if (b->_raw.hostVisible)
            b->_raw.markCpuDirty();
        useBuffer(rc, b, true);
        break;
    }
    case SwitchMetalResidencyEntry::Kind::Texture: {
        SwitchMetalTexture *t = (SwitchMetalTexture *)entry.allocation;
        // A shader reaches a texture through a residency set only via gpuResourceID (render targets and direct binds
        // name themselves on their encoders), so drawables and unbound IOSurfaces in a set ask nothing of unrelated jobs
        if (!t->_idNamed.load(std::memory_order_acquire) && !t->_viewIdNamed.load(std::memory_order_acquire))
            return;
        SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.residencyNamed++);
        __unsafe_unretained SwitchMetalTexture *root = (t->_levelsOwner && !t->_ownForm) ? t->_levelsOwner : t;
        if (entry.write && root->_surface)
            declareGpuWrites(root);
        if (root->_buffer && root->_buffer->_raw.hostVisible && !root->_buffer->_raw.wc)
            rc->clean(root->_buffer->_raw.cpu, root->_buffer->_raw.length, entry.write);
        useTexture(rc, t, entry.write);
        break;
    }
    case SwitchMetalResidencyEntry::Kind::Heap:
        SWITCH_METAL_DIAGNOSTIC(gRM->raw->stats.residencyNamed++);
        if (const auto &members = ((SwitchMetalHeap *)entry.allocation)->_members)
            rc->useHeap(*members, true);
        break;
    }
}

void gm20bNameResidencySets(SwitchMetalCommandBuffer *cb, gm20b::CommandBuffer *rc)
{
    for (const auto &snapshot : cb->_residencySnapshots)
        for (const SwitchMetalResidencyEntry &entry : snapshot->hostVisible)
            nameResident(rc, entry);
}
