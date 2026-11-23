// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "ca_route.h"
#import "TegraGM20BMetalRenderKey.h"
#import "TegraGM20BMetalBinaryArchive.h"
#import "TegraGM20BMetalReflection.h"

@implementation SwitchMetalSampler

- (NSUInteger)pixelFormat
{
    return MTLPixelFormatInvalid;  // (SPI) a sampler of a plain descriptor names no format, as m2v answers
}

- (void)dealloc
{
    gm20bHeapForgetSampler(self);
    if (gRM)
        gRM->raw->destroySampler(_raw);
}

@end

@implementation SwitchMetalPipeline

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return [(id<MTLDevice>)_device maxThreadsPerThreadgroup]
        .width;  // a tile dispatch's: the device's limit, as m2v answers
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's gpu.ca.asked_forms: whether the state's record came out of the pack at its creation
- (NSNumber *)switchPackHit
{
    return @(_packed);
}
#endif

@end

@implementation SwitchMetalDevice (States)

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    SWITCH_API(ApiObject);
    gm20b::MetalSamplerFields m;
    m.minFilter = d.minFilter;
    m.magFilter = d.magFilter;
    m.mipFilter = d.mipFilter;
    m.sAddress = d.sAddressMode;
    m.tAddress = d.tAddressMode;
    m.rAddress = d.rAddressMode;
    m.maxAnisotropy = d.maxAnisotropy;
    m.compareFunction = d.compareFunction;
    m.borderColor = d.borderColor;
    m.lodMin = d.lodMinClamp;
    m.lodMax = d.lodMaxClamp;
    m.normalizedCoordinates = d.normalizedCoordinates;
    gm20b::SamplerDesc s;
    SwitchMetalSampler *r = [SwitchMetalSampler new];
    r->_device = self;
    r->_descriptor = [d copy];
    r.label = d.label;
    // A descriptor the raw sampler header does not express is no sampler of this device's (Metal answers nil for a sampler
    // it cannot make): an enum out of range, a NaN lod clamp, or a cubic filter with a mip filter, anisotropy, a border
    // of 3 or a mirror-clamp axis, whose result Apple's GPUs are not measured for (m2v refuses it too: metal2vulkan
    // bicubic.rs runtime_cubic_measured); a header the pool has no slot for is the same answer
    const bool gm20bOk = gm20b::takeSampler(m, s);
    if (!gm20bOk || !gRM->raw->createSampler(s, r->_raw)) {
        // each descriptor the device has no form of is named once, a full table once (the fields before the flag hold
        // no padding)
        const uint64_t detail = gm20bOk
                                    ? 0
                                    : gm20b::Pack::fnv(&m, offsetof(gm20b::MetalSamplerFields, normalizedCoordinates)) ^
                                          uint64_t(m.normalizedCoordinates);
        SWITCH_REFUSE_DETAIL("gm20b", "sampler.unsupported", detail,
                             "no sampler (%s): min %lu mag %lu mip %lu address %lu/%lu/%lu anisotropy %lu compare %lu "
                             "border %lu lod %g..%g "
                             "normalized %d",
                             gm20bOk ? "no header slot" : "a descriptor the device has no form of",
                             (unsigned long)d.minFilter, (unsigned long)d.magFilter, (unsigned long)d.mipFilter,
                             (unsigned long)d.sAddressMode, (unsigned long)d.tAddressMode,
                             (unsigned long)d.rAddressMode, (unsigned long)d.maxAnisotropy,
                             (unsigned long)d.compareFunction, (unsigned long)d.borderColor, double(d.lodMinClamp),
                             double(d.lodMaxClamp), (int)d.normalizedCoordinates);
        return nil;
    }
    return (id<MTLSamplerState>)r;
}

// What Core Animation's name for a state says its draws are (ca_route.h), for the draw shortcuts' rules: the state and
// its barrier form alike
static void markRoute(gm20b::Pipeline *pl, NSString *label)
{
    static NSString *const prefix = @"com.apple.coreanimation.";
    if (!pl || pl->caRoute || ![label hasPrefix:prefix])
        return;
    const char *name = [label substringFromIndex:prefix.length].UTF8String;
    const uint8_t route = uint8_t(gm20b::caRouteOf(name));
    for (gm20b::Pipeline *p : {pl, pl->barrierTwin}) {
        if (!p)
            continue;
        p->caRoute = route;
    }
}

#if SWITCH_METAL_DIAGNOSTICS
// Core Animation's glass background (Tgs*, Tgb*) and generic merge (..Xghfc) states: the draw path checks their uniforms for
// NaN and infinity (gpu.cpp probeUniforms), its barrier form's too
static void markUniformProbe(gm20b::Pipeline *pl, NSString *label)
{
    static NSString *const prefix = @"com.apple.coreanimation.";
    if (!pl || pl->uniformProbe || ![label hasPrefix:prefix])
        return;
    NSString *name = [label substringFromIndex:prefix.length];
    const bool generic = [name hasSuffix:@"Xghfc"];
    if (![name containsString:@"_Tgs"] && ![name containsString:@"_Tgb"] && !generic)
        return;
    const char *kept = strdup(name.UTF8String);  // one per glass state, for the process's life
    pl->uniformProbe = kept;
    pl->genericState = generic;
    if (pl->barrierTwin && !pl->barrierTwin->uniformProbe) {
        pl->barrierTwin->uniformProbe = kept;
        pl->barrierTwin->genericState = generic;
    }
}

// Core Animation's states, whose draws the keyline probe logs (gpu.cpp probeDraw): a solid fill into its three-colour
// pass (..Xmw_A3Xhfc, the scroll edge keyline's) always, any other while the probe is armed
static void markDrawProbe(gm20b::Pipeline *pl, NSString *label)
{
    static NSString *const prefix = @"com.apple.coreanimation.";
    if (!pl || pl->caName || ![label hasPrefix:prefix])
        return;
    // one per state, for the process's life
    pl->caName = strdup([label substringFromIndex:prefix.length].UTF8String);
    pl->fillState = [label hasSuffix:@"Xmw_A3Xhfc"];
}

static void markProbes(gm20b::Pipeline *pl, NSString *label)
{
    markUniformProbe(pl, label);
    markDrawProbe(pl, label);
}
#endif

bool gm20bNeedsRecord(SwitchMetalPipeline *p)
{
    return !p->_raw && p->_completable && gRM->completion;
}

gm20b::Completion::Ask gm20bRecordAsk(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d)
{
    gm20b::Completion::Ask a;
    a.descriptor = d;
    a.resolveDescriptor = true;
    a.facade = d;
    a.key = p->_key;
    a.caTransform = p->_caTransform;
    a.own = !p->_packable;
    return a;
}

// A state the pack lacks, of any library's functions: its record from this process or an earlier boot, else made at its
// creation, as Metal makes a state (TegraGM20BMetalComplete.h), and the driver draws it from its first draw
static void makeRecord(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d)
{
    SWITCH_METAL_DIAGNOSTIC(p->_obtained.how = p->_raw ? "pack" : "none");
    if (!gm20bNeedsRecord(p))
        return;
    const gm20b::Completion::Ask a = gm20bRecordAsk(p, d);
    p->_raw = gRM->completion->makeNow(a, p->_failure);
    SWITCH_METAL_DIAGNOSTIC(p->_obtained = gm20b::Completion::lastObtained());
    markRoute(p->_raw, d.label);
#if SWITCH_METAL_DIAGNOSTICS
    markProbes(p->_raw, d.label);
#endif
}

// What the archives a render creation lists answer for the state of key hash `keyHash` (TegraGM20BMetalBinaryArchive.mm), Metal's
// error where they miss it
static gm20b::ArchiveAnswer renderArchiveAnswer(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, uint64_t keyHash,
                                                NSError **error)
{
    const gm20b::ArchiveAnswer answer = gm20bArchiveAnswer(d.binaryArchives, o, keyHash);
    if (answer == gm20b::ArchiveAnswer::Missed && error)
        *error = gm20bArchiveMissError((d.fragmentFunction ?: d.vertexFunction).name, "render");
    return answer;
}

// A state's functions all this device's: a fragment function of its own, or none (its pixel stage off)
static bool ownFunctions(MTLRenderPipelineDescriptor *d)
{
    return [(id)d.vertexFunction isKindOfClass:[SwitchMetalFunction class]] &&
           (!d.fragmentFunction || [(id)d.fragmentFunction isKindOfClass:[SwitchMetalFunction class]]);
}

bool gm20bRenderArchiveMissed(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error)
{
    if (!ownFunctions(d))
        return false;
    const std::string key = metal_pipeline_key(d).UTF8String;
    return renderArchiveAnswer(d, o, gm20b::Pack::fnv(key.data(), key.size()), error) == gm20b::ArchiveAnswer::Missed;
}

SwitchMetalPipeline *gm20bPipelineShell(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                        NSError **error)
{
    if (const char *why = gm20bUnsupportedRenderState(d))
        return gm20bRefuseUnsupported(why, error);
    SwitchMetalPipeline *p = [SwitchMetalPipeline new];
    p->_device = device;
    p.label = d.label;
    p->_alphaControl = gm20b::alphaControl(d.alphaToCoverageEnabled, d.alphaToOneEnabled);
    SwitchMetalFunction *vf = [(id)d.vertexFunction isKindOfClass:[SwitchMetalFunction class]]
                                  ? (SwitchMetalFunction *)d.vertexFunction
                                  : nil;
    SwitchMetalFunction *ff = [(id)d.fragmentFunction isKindOfClass:[SwitchMetalFunction class]]
                                  ? (SwitchMetalFunction *)d.fragmentFunction
                                  : nil;
    // a state linking functions is packed under them too (its key names each), where they are framework libraries' as well
    p->_completable = ownFunctions(d);
    p->_packable = p->_completable && !vf->_framework.empty() && (!ff || vf->_framework == ff->_framework) &&
                   metal_linked_packable(d);
    if (p->_completable) {
        p->_key = metal_pipeline_key(d).UTF8String;
        p->_madeFrom = [d copy];
    }
    const uint64_t keyHash = gm20b::Pack::fnv(p->_key.data(), p->_key.size());
    uint64_t packKey = keyHash;
    gm20b::CacheKey own;
    bool lookedUp = p->_packable;
    if (p->_packable) {
        p->_caTransform =
            vf->_framework == "QuartzCore" && [vf.name hasPrefix:@"fixed_vert_"] && [vf.name hasSuffix:@"_spc"];
    } else if (p->_completable && gm20b::renderCacheKey(d, p->_key, true, 0, own, gm20bFunctionSource)) {
        packKey = gm20b::Pack::fnv(own.key.data(), own.key.size());  // another library's: under its functions' AIR too
        lookedUp = true;
    }
    p->_packKey = packKey;
    if (lookedUp) {
        p->_raw = gRM->raw->pipeline(packKey, gm20b::PackForm::Default, p->_caTransform);
        markRoute(p->_raw, d.label);
#if SWITCH_METAL_DIAGNOSTICS
        markProbes(p->_raw, d.label);
#endif
        p->_packed = p->_raw && gRM->raw->packHolds(*p->_raw);
        if (p->_packed)
            packedVariants(p, packKey);
    }
    // A framework's state the pack holds is the device's archive hit, as Apple's own archives hold its states for Apple's
    // GPUs; any other state is answered by the archives the creation lists alone, as Metal answers it
    const bool deviceArchive = p->_packable && p->_raw;
    const gm20b::ArchiveAnswer answer =
        p->_completable && !deviceArchive ? renderArchiveAnswer(d, o, keyHash, error) : gm20b::ArchiveAnswer::NotAsked;
    if (answer == gm20b::ArchiveAnswer::Missed)
        return nil;
    return p;
}

SwitchMetalPipeline *gm20bPipelineFor(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                      NSError **error)
{
    SwitchMetalPipeline *p = gm20bPipelineShell(device, d, o, error);
    if (!p)
        return nil;
    makeRecord(p, d);
    if (p->_failure.empty())
        return p;
    if (error)
        *error = gm20bCreationError(p->_failure);
    return nil;
}

NSError *gm20bCreationError(const std::string &failure)
{
    return [NSError errorWithDomain:MTLLibraryErrorDomain
                               code:MTLLibraryErrorInternal
                           userInfo:@{
                               NSLocalizedDescriptionKey : @(failure.c_str())
                           }];
}

// A state of `d` under `o`: its record the pack's, or made at once, unless the archives it lists miss it as Metal's would;
// nil with Metal's error where it has none, its functions another device's or no vertex function among them (Metal's
// validation error); its reflection is raw's (TegraGM20BMetalReflection.h).
id<MTLRenderPipelineState> gm20bFinishState(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                            MTLAutoreleasedRenderPipelineReflection *reflection, NSError **error)
{
    if (reflection)
        *reflection = gm20bRenderReflection(p, d, o);
    if (!p->_raw) {
        const std::string failure = !p->_completable      ? "no vertex function, or a function not made by this device"
                                    : p->_failure.empty() ? "no shader compiler serves this process"
                                                          : p->_failure;
        os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no render state %{public}s: %{public}s", getprogname(),
                     d.label.UTF8String ?: "-", failure.c_str());
        if (error)
            *error = gm20bCreationError(failure);
        if (reflection)
            *reflection = nil;
        return nil;
    }
    if (!p->_packed && gRM->completion)
        gm20bMadeVariants(p, d);
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

static id<MTLRenderPipelineState> makeState(SwitchMetalDevice *device, MTLRenderPipelineDescriptor *d,
                                            MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *reflection,
                                            NSError **error)
{
    SwitchMetalPipeline *p = gm20bPipelineFor(device, d, o, error);
    return p ? gm20bFinishState(p, d, o, reflection, error) : nil;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (standInTargetFormat(d))
        return [self standInTargetState:d options:MTLPipelineOptionNone reflection:nil error:error];
    return makeState(self, d, MTLPipelineOptionNone, nil, error);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (standInTargetFormat(d))
        return [self standInTargetState:d options:o reflection:reflection error:error];
    return makeState(self, d, o, reflection, error);
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    return gm20bComputeState(self, d, MTLPipelineOptionNone, nil, error);
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                             error:(NSError **)error
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    return gm20bComputeState(self, d, o, r, error);
}

// _MTLDevice's own sends this to [self compiler], which answers nil on a device without Apple's compiler service: Core
// Animation 27 makes its luma kernels this way and aborts on nil.
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                               error:(NSError **)error
{
    return [self newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                             options:(MTLPipelineOption)o
                                                          reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                               error:(NSError **)error
{
    return gm20bComputeState(self, d, o, r, error);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d
                                                               options:(MTLPipelineOption)o
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)r
                                                                 error:(NSError **)error
{
    (void)d, (void)o;
    if (r)
        *r = nil;
    return gm20bRefuseUnsupported(
        "a tile render state: tile shaders are Apple4's (Metal feature set tables), a family this "
        "device does not claim",
        error);
}

- (id)newResourceGroupFromResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    return [NSObject new];  // cache hints are no-ops here (encodeCacheHint...)
}

@end
