// SPDX-License-Identifier: MIT
// The raw device's answers to Metal's capability and identity queries, which its class inherited from m2v's device class
// before: the per-kind table (raw_device_answers.h) and the identity below, each equal to m2v's answer today
// (raw/test/selcover holds them so on the host), and the identity's GPU architecture, which m2v's device answers with no
// Apple GPU subtype.
#import "RawMetalInternal.h"
#include <mach/vm_page_size.h>
#include <os/proc.h>
#include <algorithm>
#include "raw_archive.h"
#include "raw_device_answers.h"

namespace {

constexpr const raw::answers::Kind &kKind = raw::answers::kThisKind;

struct Answers {
    std::unordered_map<SEL, uint64_t> counts;
    std::unordered_map<SEL, std::vector<raw::answers::ArgRange>> args;
};

Answers &answers()
{
    static Answers *a = [] {
        auto *made = new Answers;
        // Extended-range 10-bit framebuffers are withheld from the render server (the frontend's installAnswers says why: the
        // display chain scans out 8-bit BGRX only, and backboardd would pick a 64-bit framebuffer it cannot show)
        const bool renderServer = strcmp(getprogname(), "backboardd") == 0;
        for (size_t i = 0; i < kKind.countCount; i++) {
            const raw::answers::Count &c = kKind.counts[i];
            made->counts[sel_registerName(c.selector)] =
                renderServer && !strcmp(c.selector, "supportsExtendedXR10Formats") ? 0 : c.value;
        }
        for (size_t i = 0; i < kKind.argCount; i++)
            made->args[sel_registerName(kKind.args[i].selector)].push_back(kKind.args[i]);
        return made;
    }();
    return *a;
}

NSUInteger countAnswer(id, SEL cmd)
{
    return NSUInteger(answers().counts.at(cmd));
}

BOOL flagAnswer(id, SEL cmd)
{
    return answers().counts.at(cmd) != 0;
}

NSUInteger countArgAnswer(id, SEL cmd, NSUInteger argument)
{
    return NSUInteger(deviceAnswer(cmd, argument));
}

BOOL flagArgAnswer(id, SEL cmd, NSUInteger argument)
{
    return deviceAnswer(cmd, argument) != 0;
}

}  // namespace

uint64_t deviceAnswer(SEL selector, uint64_t argument)
{
    const auto it = answers().args.find(selector);
    if (it == answers().args.end())
        return 0;
    for (const raw::answers::ArgRange &r : it->second)
        if (argument >= r.first && argument <= r.last)
            return r.value;
    return 0;
}

void installDeviceAnswers(Class cls)
{
    std::set<SEL> own;
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count; i++)
        own.insert(method_getName(methods[i]));
    free(methods);
    for (size_t i = 0; i < kKind.countCount; i++) {
        const raw::answers::Count &c = kKind.counts[i];
        SEL sel = sel_registerName(c.selector);
        if (!own.count(sel))
            class_addMethod(cls, sel, c.flag ? (IMP)flagAnswer : (IMP)countAnswer, c.flag ? "B16@0:8" : "Q16@0:8");
    }
    for (size_t i = 0; i < kKind.argCount; i++) {
        const raw::answers::ArgRange &r = kKind.args[i];
        SEL sel = sel_registerName(r.selector);
        if (!own.count(sel))
            class_addMethod(cls, sel, r.flag ? (IMP)flagArgAnswer : (IMP)countArgAnswer,
                            r.flag ? "B24@0:8Q16" : "Q24@0:8Q16");
    }
}

@interface MTLArchitecture (Private)
- (instancetype)initWithCPUType:(int)type cpuSubtype:(int)subtype;
@end

@interface RMDevice (Identity)
@end
@implementation RMDevice (Identity)

// targetDeviceArchitecture stays the base class's nil: Metal picks AGX slices of libraries and archives by it, and this GPU
// runs none of them
- (MTLArchitecture *)architecture
{
    static MTLArchitecture *const architecture = [[MTLArchitecture alloc] initWithCPUType:int(raw::kAppleGpuCpuType)
                                                                               cpuSubtype:int(kKind.gpuSubtype)];
    return architecture;
}

// The identity's GPU name, its vendor, family and product names joined as -[_MTLDevice name] joins them. Frameworks key
// Apple's per-GPU data on it: CMImaging opens its per-GPU archives only for a name containing Apple, MPSCore's
// GetMPSDevice takes its tuning by the exact name within the families supportsFamily: answers.
- (NSString *)name
{
    static NSString *const name =
        [NSString stringWithFormat:@"%.*s %.*s %.*s", int(kKind.gpuVendor.size()), kKind.gpuVendor.data(),
                                   int(kKind.gpuFamily.size()), kKind.gpuFamily.data(), int(kKind.gpuProduct.size()),
                                   kKind.gpuProduct.data()];
    return name;
}

- (NSString *)vendorName
{
    return [[NSString alloc] initWithBytes:kKind.gpuVendor.data()
                                    length:kKind.gpuVendor.size()
                                  encoding:NSUTF8StringEncoding];
}

- (NSString *)familyName
{
    return [[NSString alloc] initWithBytes:kKind.gpuFamily.data()
                                    length:kKind.gpuFamily.size()
                                  encoding:NSUTF8StringEncoding];
}

- (NSString *)productName
{
    return [[NSString alloc] initWithBytes:kKind.gpuProduct.data()
                                    length:kKind.gpuProduct.size()
                                  encoding:NSUTF8StringEncoding];
}

// The kernel's largest buffer object: MPS and MPSGraph size their tiles and scratch from it
- (NSUInteger)maxBufferLength
{
    return NSUInteger(gRM->raw->kernelInfo().max_bo_bytes);
}

- (BOOL)hasUnifiedMemory
{
    return YES;
}

- (BOOL)isLowPower
{
    return NO;
}

- (BOOL)isRemovable
{
    return NO;
}

- (BOOL)isHeadless
{
    return NO;
}

// Unified memory: all of it shared, none dedicated (Core Image sizes its caches from these)
- (uint64_t)sharedMemorySize
{
    return NSProcessInfo.processInfo.physicalMemory;
}

- (uint64_t)dedicatedMemorySize
{
    return 0;
}

// What the process holds and may still take, as m2v's device answers, within the memory there is: a process without a jetsam
// limit reads os_proc_available_memory() as about INT32_MAX MB
- (uint64_t)recommendedMaxWorkingSetSize
{
    const uint64_t physical = NSProcessInfo.processInfo.physicalMemory;
#if TARGET_OS_IPHONE
    return std::min<uint64_t>(uint64_t([(id<MTLDevice>)self currentAllocatedSize]) + os_proc_available_memory(),
                              physical);
#else
    return physical;
#endif
}

- (NSUInteger)minBufferNoCopyAlignmentBytes
{
    return NSUInteger(vm_page_size);
}

// MPS reads this private total unguarded: the public length
- (NSUInteger)maxComputeThreadgroupMemory
{
    return [(id<MTLDevice>)self maxThreadgroupMemoryLength];
}

- (MTLSize)maxThreadsPerThreadgroup
{
    return MTLSizeMake(1024, 1024, 64);
}

// The legacy feature sets of the families supportsFamily: answers (iOS GPU families 1 to 3, every version this OS defines):
// MPS's and Core Image's device checks still ask
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set
{
    const NSUInteger s = NSUInteger(set);
    return s <= 14 && s != 11;
}

// (SPI) A linear texture's row of `width` texels rounded up to the format's linear alignment, as m2v answers: 0 for a format
// of blocks, which has no texel size
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    const uint64_t align = [(id<MTLDevice>)self minimumLinearTextureAlignmentForPixelFormat:d.pixelFormat];
    if (raw::blockCompressed(f))
        return 0;
    if (!f || !align)
        return [(id<RMDeviceSPI>)RM_M2V("a linear row of a format raw does not lay out")
            requiredLinearTextureBytesPerRowForDescriptor:d];
    return NSUInteger((uint64_t(d.width) * f->bpp + align - 1) / align * align);
}

// The linear alignment m2v's texel rows take (IconRendering's textures over buffers)
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format
{
    return NSUInteger(deviceAnswer(@selector(minimumLinearTextureAlignmentForPixelFormat:), format));
}

- (BOOL)supportsDynamicLibraries
{
    return NO;
}

// A pipeline library (RenderBox's default.pipelinelib, PencilKit's...) holds Apple GPUs' compiled pipelines, none for this one:
// refused as Metal refuses one without the device's file, and its caller makes its states from its metallib at once
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
    SWITCH_REFUSE("raw", "pipelinelibrary", "pipeline library %s: none for this GPU (file not found, as Metal answers)",
                  path.lastPathComponent.UTF8String ?: "-");
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorFileNotFound
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         @"a pipeline library holds Apple GPUs' pipelines, none for this device"
                                 }];
    return nil;
}

- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    id<MTLDevice> device = (id<MTLDevice>)self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (handler)
            handler(library, error);
    });
}

@end
