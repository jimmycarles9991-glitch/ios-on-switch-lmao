// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

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
    if (gRM)
        gRM->raw->destroySampler(_raw);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [gRM->fallback newSamplerStateWithDescriptor:_descriptor]);
}

@end

@implementation RMPipeline

+ (Class)twinClass
{
    return NSClassFromString(@"NBRenderPipeline");
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return [(id<MTLDevice>)_device maxThreadsPerThreadgroup]
        .width;  // a tile dispatch's: the device's limit, as m2v answers
}

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

@implementation RMDepthStencil
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

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    id twin = [gRM->fallback newDepthStencilStateWithDescriptor:d];
    if (!twin)
        return nil;
    RMDepthStencil *o = [RMDepthStencil new];
    o->_device = self;
    o->_twin = twin;
    o.label = d.label;
    o->_raw.depthCompare = uint8_t(d.depthCompareFunction);
    o->_raw.depthWrite = d.depthWriteEnabled;
    raw::StencilFace *faces[2] = {&o->_raw.front, &o->_raw.back};
    MTLStencilDescriptor *stencils[2] = {d.frontFaceStencil, d.backFaceStencil};
    for (int i = 0; i < 2; i++)
        if (MTLStencilDescriptor *s = stencils[i]) {
            faces[i]->compare = uint8_t(s.stencilCompareFunction);
            faces[i]->fail = uint8_t(s.stencilFailureOperation);
            faces[i]->depthFail = uint8_t(s.depthFailureOperation);
            faces[i]->pass = uint8_t(s.depthStencilPassOperation);
            faces[i]->readMask = s.readMask;
            faces[i]->writeMask = s.writeMask;
        }
    return (id<MTLDepthStencilState>)o;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithURL:url error:error], url.path);
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithFile:path error:error], path);
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithData:data error:error], nil);
}

- (id<MTLLibrary>)newDefaultLibrary
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibrary], nil);
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newDefaultLibraryWithBundle:bundle error:error],
                                       [bundle.bundlePath stringByAppendingString:@"/default.metallib"]);
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithSource:source options:options error:error],
                                       nil);
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

// A descriptor whose functions are this device's: a copy over their twins, for the m2v device
static MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d)
{
    MTLRenderPipelineDescriptor *t = [d copy];
    t.vertexFunction = twinOf(d.vertexFunction);
    t.fragmentFunction = twinOf(d.fragmentFunction);
    t.vertexLinkedFunctions = twinLinked(d.vertexLinkedFunctions);
    t.fragmentLinkedFunctions = twinLinked(d.fragmentLinkedFunctions);
    return t;
}

// A state the pack lacks, of any library's functions: its record from this process or an earlier boot, else made at its
// creation, as Metal makes a state (RawComplete.h), and the raw path draws it from its first draw. A compile that could not
// run is asked again off this thread; m2v draws the state until then.
static void makeRecord(RMPipeline *p, MTLRenderPipelineDescriptor *d)
{
    if (p->_raw || !p->_completable || !gRM->completion)
        return;
    raw::Completion::Ask a;
    a.descriptor = twinDescriptor(d);
    a.key = p->_key;
    a.caTransform = p->_caTransform;
    a.own = !p->_packable;
    bool retry = false;
    if ((p->_raw = gRM->completion->makeNow(a, &retry)) || !retry)
        return;
    p->_asked = true;
    __weak RMPipeline *weak = p;
    gRM->completion->ask(std::move(a), [weak](raw::Pipeline *pl) {
        RMPipeline *strong = weak;
        if (strong && pl)
            __atomic_store_n(&strong->_raw, pl, __ATOMIC_RELEASE);
        else if (strong)
            strong->_asked = false;
    });
}

- (RMPipeline *)pipelineFor:(MTLRenderPipelineDescriptor *)d twin:(id<MTLRenderPipelineState>)twin
{
    RMPipeline *p = [RMPipeline new];
    p->_device = self;
    p->_twin = twin;
    p.label = d.label;
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    // a state linking functions is packed under them too (its key names each), where they are framework libraries' as well
    p->_packable =
        vf && ff && !vf->_framework.empty() && vf->_framework == ff->_framework && raw_metal_linked_packable(d);
    p->_completable = vf && ff;
    if (p->_completable)
        p->_key = raw_metal_pipeline_key(d).UTF8String;
    if (p->_packable) {
        p->_caTransform =
            vf->_framework == "QuartzCore" && [vf.name hasPrefix:@"fixed_vert_"] && [vf.name hasSuffix:@"_spc"];
        p->_raw =
            gRM->raw->pipeline(raw::Pack::fnv(p->_key.data(), p->_key.size()), raw::PackForm::Default, p->_caTransform);
    }
    makeRecord(p, d);
    if (!p->_raw) {
        const char *label = d.label.UTF8String ?: "-";
        SWITCH_REFUSE_DETAIL("raw", "state.unpacked", raw::Pack::fnv(label, strlen(label)),
                             "state neither in the pack nor made at its creation: m2v draws it%s: %s",
                             !p->_completable           ? " (a function not this device's: never made)"
                             : p->_asked                ? " until its compile, asked again, makes it"
                             : gRM->raw->stats.heapFull ? " (or the shader heap is full)"
                                                        : " (the raw log names why it was refused)",
                             label);
    }
    return p;
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
    return packed ? o & ~MTLPipelineOptionFailOnBinaryArchiveMiss : o;
}

static id<MTLRenderPipelineState> pipelineTwin(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, bool packed,
                                               NSError **error)
{
    if (packed && native_bridge_metal_render_pipeline_lazy)
        return native_bridge_metal_render_pipeline_lazy(gRM->fallback, packedTwinDescriptor(d, true),
                                                        packedTwinOptions(o, true));
    return [gRM->fallback newRenderPipelineStateWithDescriptor:twinDescriptor(d) options:o reflection:nil error:error];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    RMPipeline *p = [self pipelineFor:d twin:nil];
    p->_twin = pipelineTwin(d, MTLPipelineOptionNone, p->_raw, error);
    if (!p->_twin && !p->_raw)
        return nil;
    if (error && p->_raw)
        *error = nil;
    return (id<MTLRenderPipelineState>)p;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    SWITCH_API(ApiObject);
    if (reflection) {
        RMPipeline *p = [self pipelineFor:d twin:nil];
        const bool packed = p->_raw != nullptr;
        p->_twin = [gRM->fallback newRenderPipelineStateWithDescriptor:packedTwinDescriptor(d, packed)
                                                               options:packedTwinOptions(o, packed)
                                                            reflection:reflection
                                                                 error:error];
        if (error && p->_raw)
            *error = nil;
        return !p->_twin && !p->_raw ? nil : (id<MTLRenderPipelineState>)p;
    }
    RMPipeline *p = [self pipelineFor:d twin:nil];
    p->_twin = pipelineTwin(d, o, p->_raw, error);
    if (error && p->_raw)
        *error = nil;
    return !p->_twin && !p->_raw ? nil : (id<MTLRenderPipelineState>)p;
}

// Made as the synchronous calls make a state (its record at its creation, its m2v twin at its first fallback), on a global queue
// of the caller's class, which Metal calls the handler on
static dispatch_queue_t creationQueue()
{
    const qos_class_t mine = qos_class_self();
    return dispatch_get_global_queue(mine == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INITIATED : mine, 0);
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    dispatch_async(creationQueue(), ^{
        NSError *e = nil;
        id<MTLRenderPipelineState> state = [me newRenderPipelineStateWithDescriptor:copy error:&e];
        handler(state, e);
    });
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                     options:(MTLPipelineOption)o
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    RMDevice *me = self;
    dispatch_async(creationQueue(), ^{
        NSError *e = nil;
        MTLAutoreleasedRenderPipelineReflection reflection = nil;
        id<MTLRenderPipelineState> state = [me
            newRenderPipelineStateWithDescriptor:copy
                                         options:o
                                      reflection:(o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo))
                                                     ? &reflection
                                                     : nil
                                           error:&e];
        handler(state, reflection, e);
    });
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
    return rmComputeState(self, [gRM->fallback newComputePipelineStateWithFunction:twinOf(f) error:error], f, d);
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f
                                                           options:(MTLPipelineOption)o
                                                        reflection:(MTLAutoreleasedComputePipelineReflection *)r
                                                             error:(NSError **)error
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    return rmComputeState(
        self, [gRM->fallback newComputePipelineStateWithFunction:twinOf(f) options:o reflection:r error:error], f, d);
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
    MTLComputePipelineDescriptor *t = [d copy];
    t.computeFunction = twinOf(d.computeFunction);
    return rmComputeState(self,
                          [gRM->fallback newComputePipelineStateWithDescriptor:t options:o reflection:r error:error],
                          d.computeFunction, d);
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d
                                                               options:(MTLPipelineOption)o
                                                            reflection:(MTLAutoreleasedRenderPipelineReflection *)r
                                                                 error:(NSError **)error
{
    MTLTileRenderPipelineDescriptor *t = [d copy];
    t.tileFunction = twinOf(d.tileFunction);
    return [self wrapCarried:[gRM->fallback newRenderPipelineStateWithTileDescriptor:t
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
    return (id<MTLLibrary>)wrapLibrary(self, [gRM->fallback newLibraryWithStitchedDescriptor:t error:error], nil);
}

- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error
{
    NSArray *twins = twinFunctions(functions, error);
    if (!twins)
        return nil;
    return (id<MTLLibrary>)wrapLibrary(self,
                                       [(id<RMImageFilterLinking>)gRM->fallback newLibraryWithCIFilters:twins
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
