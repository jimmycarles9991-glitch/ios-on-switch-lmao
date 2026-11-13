// SPDX-License-Identifier: MIT
// What the raw device class inherited from m2v's device class (the frontend's NBDeviceMethods) and answers itself now that it
// is an _MTLDevice of its own: the four calls Metal's registration makes, the identity and memory queries, a residency set,
// and the library forms the frontend sent back to the device (to the facade's own methods, which open m2v for what needs it).
#import "RawMetalInternal.h"
#include "raw_limits_fill.h"

// The base class's limits table (LimitsTable.h): _MTLDevice's own accessors read it, and frameworks read it past them
// (PhotosUICore's 2D texture size, CoreRE's no-copy alignment); each field the raw device answers with a method of its own
// is written, as the frontend's fillLimits writes m2v's. Every field m2v's device answered the raw device answers too
// (raw/test/selcover's capability check; RawMetalM2V.mm opens m2v for any it has none for). The host's _MTLDevice has a
// layout of its own.
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
            SWITCH_REFUSE_DETAIL("raw", "limits.layout", field.offset,
                                 "limits: %s not written at 0x%x (_MTLDevice does not read it there)", field.selector,
                                 field.offset);
    });
#else
    (void)device;
#endif
}

// The facade's own Core Image filter-function linking (RawMetalStates.mm)
@protocol RMFilterLibraries
- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error;
@end

API_AVAILABLE(ios(18.0), macos(15.0))
@interface RMResidencySet : RMObject <MTLResidencySet>
- (instancetype)initWithDescriptor:(MTLResidencySetDescriptor *)descriptor device:(id)device;
@end

@interface RMDevice (Base)
@end
@implementation RMDevice (Base)

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

// The GPU memory this device's objects hold: the raw path's allocations and, once it is open, the m2v device's
- (NSUInteger)currentAllocatedSize
{
    uint64_t m2v = 0;
    if (rmM2VIfOpen())
        m2v = [rmM2VIfOpen() currentAllocatedSize];
    return NSUInteger(gRM->raw->allocatedBytes() + m2v);
}

// Every allocation is resident from its creation to its release (one GPU address space, no paging): a residency set keeps
// its allocations for its client and asks nothing of the GPU, as m2v's does
- (id<MTLResidencySet>)newResidencySetWithDescriptor:(MTLResidencySetDescriptor *)descriptor
                                               error:(NSError **)error API_AVAILABLE(ios(18.0), macos(15.0))
{
    if (error)
        *error = nil;
    return [[RMResidencySet alloc] initWithDescriptor:descriptor device:self];
}

// Function stitching from Core Image's DAG form, which m2v refuses
- (id<MTLLibrary>)newLibraryWithDAG:(id)dag functions:(NSArray *)functions error:(NSError **)error
{
    (void)dag;
    (void)functions;
    SWITCH_REFUSE("raw", "library.dag", "function stitching (DAG) refused, as m2v refuses it");
    if (error)
        *error =
            [NSError errorWithDomain:@"org.blu-sharky.native-metal"
                                code:-1
                            userInfo:@{NSLocalizedDescriptionKey : @"function stitching (DAG) is not implemented"}];
    return nil;
}

// _MTLDevice sends Core Image's legacy DAG and a kernel's constants reflection here
- (id<MTLLibrary>)newLibraryWithImageFilterFunctionsSPI:(NSArray *)functions
                                imageFilterFunctionInfo:(const void *)info
                                                  error:(NSError **)error
{
    return [(id<RMFilterLibraries>)self newLibraryWithCIFilters:functions imageFilterFunctionInfo:info error:error];
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

@end

@implementation RMResidencySet {
    raw::Mutex _lock;
    NSMutableArray<id<MTLAllocation>> *_pending;
    NSArray<id<MTLAllocation>> *_committed;
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
    std::lock_guard<raw::Mutex> hold(_lock);
    uint64_t bytes = 0;
    for (id<MTLAllocation> allocation in _committed)
        bytes += allocation.allocatedSize;
    return bytes;
}

- (void)addAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<raw::Mutex> hold(_lock);
    if (allocation && ![_pending containsObject:allocation])
        [_pending addObject:allocation];
}

- (void)addAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    for (NSUInteger i = 0; i < count; i++)
        [self addAllocation:allocations[i]];
}

- (void)removeAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<raw::Mutex> hold(_lock);
    [_pending removeObject:allocation];
}

- (void)removeAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    for (NSUInteger i = 0; i < count; i++)
        [self removeAllocation:allocations[i]];
}

- (void)removeAllAllocations
{
    std::lock_guard<raw::Mutex> hold(_lock);
    [_pending removeAllObjects];
}

- (BOOL)containsAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<raw::Mutex> hold(_lock);
    return [_pending containsObject:allocation];
}

- (NSArray<id<MTLAllocation>> *)allAllocations
{
    std::lock_guard<raw::Mutex> hold(_lock);
    return [_pending copy];
}

- (NSUInteger)allocationCount
{
    std::lock_guard<raw::Mutex> hold(_lock);
    return _pending.count;
}

- (void)commit
{
    std::lock_guard<raw::Mutex> hold(_lock);
    _committed = [_pending copy];
}

- (void)requestResidency
{}

- (void)endResidency
{}

@end
