// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_blur_twod.h"
#include "raw_ca_route.h"
#import "RawRenderKey.h"
#import "RawMetalBinaryArchive.h"

// The m2v device's Core Image filter-function linking (_MTLDevice's newLibraryWithImageFilterFunctionsSPI: sends it here)
@protocol RMImageFilterLinking
- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error;
@end

@implementation RMSampler

+ (Class)twinClass
{
    return NSClassFromString(@"NBSampler");
}

- (NSUInteger)pixelFormat
{
    return MTLPixelFormatInvalid;  // (SPI) a sampler of a plain descriptor names no format, as m2v answers
}

- (void)dealloc
{
    rmHeapForgetSampler(self);
    if (gRM)
        gRM->raw->destroySampler(_raw);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [RM_M2V("a sampler's twin") newSamplerStateWithDescriptor:_descriptor]);
}

@end

static id<MTLRenderPipelineState> laterTwin(MTLRenderPipelineDescriptor *d, MTLPipelineOption o);

@implementation RMPipeline

+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderPipeline");
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin || !_twinLater)
            return _twin;
    }
    return twinPublish(self, _twin, laterTwin(_madeFrom, _twinOptions));
}

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

@implementation RMTwinObject

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    return [super conformsToProtocol:protocol] || [_twin conformsToProtocol:protocol];
}

// The m2v object it carries (made with it, never for a selector) answers what this class does not: to its callers it is that
// object (a buffer the raw pool had no room for, a sampler or tile state the raw path cannot draw with)
- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return _twin;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    return [super respondsToSelector:selector] || [_twin respondsToSelector:selector];
}

@end

@implementation RMDevice (States)

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    SWITCH_API(ApiObject);
    raw::MetalSamplerFields m;
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
    raw::SamplerDesc s;
    RMSampler *r = [RMSampler new];
    r->_device = self;
    r->_descriptor = [d copy];
    r.label = d.label;
    // what the raw sampler header does not express: the m2v twin is the sampler, and the raw path refuses the draws that use it
    const bool rawOk = raw::takeSampler(m, s);
    if (!rawOk || !gRM->raw->createSampler(s, r->_raw)) {
        if (!predLogged(PredCallSamplerForeign))
            logPred(
                PredCallSamplerForeign, d.maxAnisotropy, d.compareFunction,
                [NSString
                    stringWithFormat:@"(made: normalized %d address %lu/%lu border %lu lodAverage %d header slot %d)",
                                     (int)d.normalizedCoordinates, (unsigned long)d.sAddressMode,
                                     (unsigned long)d.tAddressMode, (unsigned long)d.borderColor, (int)d.lodAverage,
                                     (int)rawOk]);
        if (![r rmTwin])
            return nil;
        RMTwinObject *o =
            [RMTwinObject new];  // a sampler the raw path cannot use: a draw with it goes to m2v (not an RMSampler)
        o->_device = self;
        o->_twin = r->_twin;
        return (id<MTLSamplerState>)o;
    }
    return (id<MTLSamplerState>)r;
}

// A stage's linked functions as their twins; `l` itself while one has none (m2v then refuses it as another device's)
static MTLLinkedFunctions *twinLinked(MTLLinkedFunctions *l)
{
    if (!l.functions.count && !l.privateFunctions.count)
        return l;
    NSMutableArray *functions = [NSMutableArray array], *hidden = [NSMutableArray array];
    for (id f in l.functions) {
        id twin = twinOf(f);
        if (!twin)
            return l;
        [functions addObject:twin];
    }
    for (id f in l.privateFunctions) {
        id twin = twinOf(f);
        if (!twin)
            return l;
        [hidden addObject:twin];
    }
    MTLLinkedFunctions *t = [MTLLinkedFunctions linkedFunctions];
    t.functions = functions;
    t.privateFunctions = hidden;
    t.binaryFunctions = l.binaryFunctions;
    t.groups = l.groups;
    return t;
}

MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)
{
    MTLRenderPipelineDescriptor *t = [d copy];
    t.binaryArchives = nil;  // this device's (RawMetalBinaryArchive.mm), which m2v cannot read
    t.vertexFunction = twinOf(d.vertexFunction);
    t.fragmentFunction = twinOf(d.fragmentFunction);
    t.vertexLinkedFunctions = twinLinked(d.vertexLinkedFunctions);
    t.fragmentLinkedFunctions = twinLinked(d.fragmentLinkedFunctions);
    return t;
}

// What Core Animation's name for a state says its draws are (raw_ca_route.h), for the draw shortcuts' rules: the state and
// its barrier form alike
static void markRoute(raw::Pipeline *pl, NSString *label)
{
    static NSString *const prefix = @"com.apple.coreanimation.";
    if (!pl || pl->caRoute || ![label hasPrefix:prefix])
        return;
    const char *name = [label substringFromIndex:prefix.length].UTF8String;
    const uint8_t route = uint8_t(raw::caRouteOf(name)), blur = uint8_t(raw::blurFunctionOf(name));
    for (raw::Pipeline *p : {pl, pl->barrierTwin}) {
        if (!p)
            continue;
        p->caRoute = route;
        p->blurFunction = blur;
    }
}

#if SWITCH_RAW_DIAGNOSTICS
// Core Animation's glass background (Tgs*, Tgb*) and generic merge (..Xghfc) states: the draw path checks their uniforms for
// NaN and infinity (raw_gpu.cpp probeUniforms), its barrier form's too
static void markUniformProbe(raw::Pipeline *pl, NSString *label)
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

// Core Animation's states, whose draws the keyline probe logs (raw_gpu.cpp probeDraw): a solid fill into its three-colour
// pass (..Xmw_A3Xhfc, the scroll edge keyline's) always, any other while the probe is armed
static void markDrawProbe(raw::Pipeline *pl, NSString *label)
{
    static NSString *const prefix = @"com.apple.coreanimation.";
    if (!pl || pl->caName || ![label hasPrefix:prefix])
        return;
    // one per state, for the process's life
    pl->caName = strdup([label substringFromIndex:prefix.length].UTF8String);
    pl->fillState = [label hasSuffix:@"Xmw_A3Xhfc"];
}

static void markProbes(raw::Pipeline *pl, NSString *label)
{
    markUniformProbe(pl, label);
    markDrawProbe(pl, label);
}
#endif

bool rmNeedsRecord(RMPipeline *p)
{
    return !p->_raw && p->_completable && gRM->completion;
}

raw::Completion::Ask rmRecordAsk(RMPipeline *p, MTLRenderPipelineDescriptor *d)
{
    raw::Completion::Ask a;
    a.descriptor = d;
    a.resolveDescriptor = true;
    a.facade = d;
    a.key = p->_key;
    a.caTransform = p->_caTransform;
    a.own = !p->_packable;
    return a;
}

// A state the pack lacks, of any library's functions: its record from this process or an earlier boot, else made at its
// creation, as Metal makes a state (RawComplete.h), and the raw path draws it from its first draw
static void makeRecord(RMPipeline *p, MTLRenderPipelineDescriptor *d)
{
    RAW_DIAGNOSTIC(p->_obtained.how = p->_raw ? "pack" : "none");
    if (!rmNeedsRecord(p))
        return;
    const raw::Completion::Ask a = rmRecordAsk(p, d);
    p->_raw = gRM->completion->makeNow(a, p->_failure);
    RAW_DIAGNOSTIC(p->_obtained = raw::Completion::lastObtained());
#if SWITCH_RAW_DIAGNOSTICS
    // the journal names m2v's functions (the harvest's corpus lines), so it is written only where m2v's device is open
    // already: a process without one is left to tools/gpu/logharvest.py, which reads its compile lines
    if (p->_raw && native_bridge_metal_journal_render && rmM2VIfOpen())
        native_bridge_metal_journal_render(twinDescriptor(a.descriptor), 0);
#endif
    markRoute(p->_raw, d.label);
#if SWITCH_RAW_DIAGNOSTICS
    markProbes(p->_raw, d.label);
#endif
}

// What the archives a render creation lists answer for the state of key hash `keyHash` (RawMetalBinaryArchive.mm), Metal's
// error where they miss it
static raw::ArchiveAnswer renderArchiveAnswer(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, uint64_t keyHash,
                                              NSError **error)
{
    const raw::ArchiveAnswer answer = rmArchiveAnswer(d.binaryArchives, o, keyHash);
    if (answer == raw::ArchiveAnswer::Missed && error)
        *error = rmArchiveMissError((d.fragmentFunction ?: d.vertexFunction).name, "render");
    return answer;
}

bool rmRenderArchiveMissed(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error)
{
    if (![(id)d.vertexFunction isKindOfClass:[RMFunction class]] ||
        ![(id)d.fragmentFunction isKindOfClass:[RMFunction class]])
        return false;
    const std::string key = raw_metal_pipeline_key(d).UTF8String;
    return renderArchiveAnswer(d, o, raw::Pack::fnv(key.data(), key.size()), error) == raw::ArchiveAnswer::Missed;
}

RMPipeline *rmPipelineShell(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error)
{
    if (const char *why = rmUnsupportedRenderState(d))
        return rmRefuseUnsupported(why, error);
    RMPipeline *p = [RMPipeline new];
    p->_device = device;
    p.label = d.label;
    p->_m2vMissing = rmM2vMissingState(d);
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    // a state linking functions is packed under them too (its key names each), where they are framework libraries' as well
    p->_packable =
        vf && ff && !vf->_framework.empty() && vf->_framework == ff->_framework && raw_metal_linked_packable(d);
    p->_completable = vf && ff;
    if (p->_completable) {
        p->_key = raw_metal_pipeline_key(d).UTF8String;
        p->_madeFrom = [d copy];
    }
    const uint64_t keyHash = raw::Pack::fnv(p->_key.data(), p->_key.size());
    uint64_t packKey = keyHash;
    raw::CacheKey own;
    bool lookedUp = p->_packable;
    if (p->_packable) {
        p->_caTransform =
            vf->_framework == "QuartzCore" && [vf.name hasPrefix:@"fixed_vert_"] && [vf.name hasSuffix:@"_spc"];
    } else if (p->_completable && raw::renderCacheKey(d, p->_key, true, 0, own, rmFunctionSource)) {
        packKey = raw::Pack::fnv(own.key.data(), own.key.size());  // another library's: under its functions' AIR too
        lookedUp = true;
    }
    if (lookedUp) {
        p->_raw = gRM->raw->pipeline(packKey, raw::PackForm::Default, p->_caTransform);
        markRoute(p->_raw, d.label);
#if SWITCH_RAW_DIAGNOSTICS
        markProbes(p->_raw, d.label);
#endif
        p->_packed = p->_raw && gRM->raw->packHolds(*p->_raw);
        if (p->_packed)
            packedVariants(p, packKey);
    }
    // A framework's state the pack holds is the device's archive hit, as Apple's own archives hold its states for Apple's
    // GPUs; any other state is answered by the archives the creation lists alone, as Metal answers it
    const bool deviceArchive = p->_packable && p->_raw;
    const raw::ArchiveAnswer answer =
        p->_completable && !deviceArchive ? renderArchiveAnswer(d, o, keyHash, error) : raw::ArchiveAnswer::NotAsked;
    if (answer == raw::ArchiveAnswer::Missed)
        return nil;
    p->_archived = answer == raw::ArchiveAnswer::Declared;
    return p;
}

RMPipeline *rmPipelineFor(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error)
{
    RMPipeline *p = rmPipelineShell(device, d, o, error);
    if (!p)
        return nil;
    makeRecord(p, d);
    if (p->_failure.empty())
        return p;
    if (error)
        *error = rmCreationError(p->_failure);
    return nil;
}

NSError *rmCreationError(const std::string &failure)
{
    return [NSError errorWithDomain:MTLLibraryErrorDomain
                               code:MTLLibraryErrorInternal
                           userInfo:@{
                               NSLocalizedDescriptionKey : @(failure.c_str())
                           }];
}

static void refuseUnmade(RMPipeline *p, MTLRenderPipelineDescriptor *d)
{
    if (p->_raw)
        return;
    const char *label = d.label.UTF8String ?: "-";
    SWITCH_REFUSE_DETAIL("raw", "state.unpacked", raw::Pack::fnv(label, strlen(label)),
                         "state neither in the pack nor made at its creation: m2v draws it%s: %s (functions %s, %s)",
                         !p->_completable ? " (a function not this device's: never made)"
                                          : " (the raw log names why it was refused)",
                         label, d.vertexFunction ? object_getClassName(d.vertexFunction) : "nil",
                         d.fragmentFunction ? object_getClassName(d.fragmentFunction) : "nil");
}

// m2v's state over the twins, for a command buffer that has to go to m2v (made asynchronously in the render server:
// AsyncPipelines.h). A state the pack draws gets one made at its first set there, by a fallback: made eagerly on the
// user-initiated workers (above backboardd's main thread), they cost ~9 ms of CPU each, 25-170 ms of a worker, while no
// command buffer fell back (11-06 21:03: 61 twins, 1074 raw command buffers, none to m2v).
// A packed state is the device's archive hit, so its twin goes without the archives the creation listed (Core Animation's
// own, which holds nothing for this GPU) and without FailOnBinaryArchiveMiss: refused there, a command buffer that falls
// back would have no state to draw with, and listed there, the twin would be made at once instead of at its first set.
// Every way a state is made (with or without reflection, at once or through a completion handler) asks m2v for its twin
// this way. A twin m2v refused leaves a command buffer that falls back with no state: m2v fails all of it ("foreign
// render pipeline", device boot 10: 833 of Core Animation's command buffers, each leaving its framebuffer a frame
// behind).
static MTLRenderPipelineDescriptor *packedTwinDescriptor(MTLRenderPipelineDescriptor *d, bool packed)
{
    MTLRenderPipelineDescriptor *t = twinDescriptor(d);
    if (packed)
        t.binaryArchives = nil;
    return t;
}

static MTLPipelineOption packedTwinOptions(MTLPipelineOption o, bool packed)
{
    (void)packed;  // the facade answers every archive miss (RawMetalBinaryArchive.mm)
    return o & ~MTLPipelineOptionFailOnBinaryArchiveMiss;
}

static id<MTLRenderPipelineState> pipelineTwin(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool packed,
                                               NSError **error)
{
    if (packed && native_bridge_metal_render_pipeline_lazy)
        return native_bridge_metal_render_pipeline_lazy(RM_M2V("a packed render state's twin"),
                                                        packedTwinDescriptor(d, true), packedTwinOptions(o, true));
    return [RM_M2V("a render state's twin") newRenderPipelineStateWithDescriptor:twinDescriptor(d)
                                                                         options:packedTwinOptions(o, packed)
                                                                      reflection:nil
                                                                           error:error];
}

// The twin of a state raw drew from its creation, made when a command buffer that sets it first goes to m2v
static id<MTLRenderPipelineState> laterTwin(MTLRenderPipelineDescriptor *d, MTLPipelineOption o)
{
    id<MTLDevice> m2v = RM_M2V("a raw state's twin, at its first m2v replay");
    if (native_bridge_metal_render_pipeline_lazy)
        return native_bridge_metal_render_pipeline_lazy(m2v, packedTwinDescriptor(d, true), packedTwinOptions(o, true));
    return [m2v newRenderPipelineStateWithDescriptor:packedTwinDescriptor(d, true)
                                             options:packedTwinOptions(o, true)
                                          reflection:nil
                                               error:nil];
}

// A state of `d` under `o`: its record the pack's, or made at once, unless the archives it lists miss it as Metal's would; its
// m2v twin made as a packed state's where the pack or an archive holds it (packedTwinDescriptor). A state raw draws from
// its creation, asked for no reflection, gets its twin only if a command buffer that sets it goes to m2v.
id<MTLRenderPipelineState> rmFinishState(RMPipeline *p, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                         MTLAutoreleasedRenderPipelineReflection *reflection, NSError **error)
{
    const bool packed = p->_raw || p->_archived;
    if (p->_raw && p->_madeFrom && !reflection) {
        p->_twinOptions = o;
        p->_twinLater = true;
        if (error)
            *error = nil;
        return (id<MTLRenderPipelineState>)p;
    }
    if (reflection) {
        id<MTLDevice> m2v = RM_M2V("a render state's twin with its reflection");
        p->_twin = [m2v newRenderPipelineStateWithDescriptor:packedTwinDescriptor(d, packed)
                                                     options:packedTwinOptions(o, packed)
                                                  reflection:reflection
                                                       error:error];
    } else {
        p->_twin = pipelineTwin(d, o, packed, error);
    }
    refuseUnmade(p, d);
    if (error && p->_raw)
        *error = nil;
    return !p->_twin && !p->_raw ? nil : (id<MTLRenderPipelineState>)p;
}

static id<MTLRenderPipelineState> makeState(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o,
                                            MTLAutoreleasedRenderPipelineReflection *reflection, NSError **error)
{
    RMPipeline *p = rmPipelineFor(device, d, o, error);
    return p ? rmFinishState(p, d, o, reflection, error) : nil;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (ycbcrTargetFormat(d))
        return [self ycbcrTargetState:d options:MTLPipelineOptionNone reflection:nil error:error];
    return makeState(self, d, MTLPipelineOptionNone, nil, error);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (ycbcrTargetFormat(d))
        return [self ycbcrTargetState:d options:o reflection:reflection error:error];
    return makeState(self, d, o, reflection, error);
}

- (id)wrapCarried:(id)twin
{
    if (!twin)
        return nil;
    RMTwinObject *o = [RMTwinObject new];
    o->_device = self;
    o->_twin = twin;
    return o;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    return rmComputeState(self, d, MTLPipelineOptionNone, nil, error);
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                             error:(NSError **)error
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    return rmComputeState(self, d, o, r, error);
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
    return rmComputeState(self, d, o, r, error);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d
                                                               options:(MTLPipelineOption)o
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)r
                                                                 error:(NSError **)error
{
    MTLTileRenderPipelineDescriptor *t = [d copy];
    t.tileFunction = twinOf(d.tileFunction);
    if (!rmTileArchiveAnswer(t, d, o, error))
        return nil;
    return [self wrapCarried:[RM_M2V("a tile render state") newRenderPipelineStateWithTileDescriptor:t
                                                                                             options:o
                                                                                          reflection:r
                                                                                               error:error]];
}

// The m2v device links these libraries from its own functions' bitcode: the functions here are their twins. Its methods, inherited
// by this class, would be sent this device's functions.
static NSArray *twinFunctions(NSArray *functions, NSError **error)
{
    NSMutableArray *twins = [NSMutableArray arrayWithCapacity:functions.count];
    for (id f in functions) {
        id twin = twinOf(f);
        if (!twin) {
            if (error)
                *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                             code:MTLLibraryErrorFunctionNotFound
                                         userInfo:@{NSLocalizedDescriptionKey : @"a function with no m2v twin"}];
            return nil;
        }
        [twins addObject:twin];
    }
    return twins;
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    NSArray *twins = twinFunctions(d.functions, error);
    if (!twins)
        return nil;
    MTLStitchedLibraryDescriptor *t = [d copy];
    t.functions = twins;
    id<MTLDevice> m2v = RM_M2V("a stitched library");
    return (id<MTLLibrary>)wrapLibrary(self, [m2v newLibraryWithStitchedDescriptor:t error:error], nil);
}

- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error
{
    NSArray *twins = twinFunctions(functions, error);
    if (!twins)
        return nil;
    id<MTLDevice> m2v = RM_M2V("a Core Image filter library");
    return (id<MTLLibrary>)wrapLibrary(self,
                                       [(id<RMImageFilterLinking>)m2v newLibraryWithCIFilters:twins
                                                                      imageFilterFunctionInfo:info
                                                                                        error:error],
                                       nil);
}

- (id)newResourceGroupFromResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    return [NSObject new];  // cache hints are no-ops here (encodeCacheHint...)
}

@end
