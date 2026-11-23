// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BMetalComplete.h"
#include "TegraGM20BMetalDirect.h"
#include "compute.h"
#include "compute_key.h"
#include "compute_lookup.h"
#include "decoded.h"
#include "kernel_texture.h"
#import "TegraGM20BMetalComputeState.h"
#import "TegraGM20BMetalResolveAsk.h"
#include "predication.h"
#import "TegraGM20BMetalCounters.h"
#include "TegraGM20BClockABI.h"
#import "TegraGM20BMetalIndirectCompute.h"
#include "view.h"
#include "surface_view.h"

using SwitchMetalKernelJob = gm20b::ComputeKernelJob<gm20b::ComputeState>;

// An RGB10A8_2P_XR10 texture held in Metal's two planes (xr_pass.h), whose kernels read and write each plane
static bool heldInPlanes(SwitchMetalTexture *t)
{
    return t->_gm20bOk && t->_raw.xrPlanes && !t->_raw.memoryless;
}

@interface SwitchMetalComputeEncoder () {
    SwitchMetalComputeState *_state;
    gm20b::ComputeSpecialization _specialization;
    std::shared_ptr<SwitchMetalKernelSet> _kernel;
    gm20b::ComputeDispatch _shape;
    bool _kernelCurrent;
    // what the dispatches' kernels may read (unretained: the command buffer retains them): the textures, and the buffers'
    // addresses (0: setBytes:, which the driver uploads aligned)
    SwitchMetalTexture *_textures[31];
    SwitchMetalSampler *_samplers[16];
    uint32_t _storableStandIns;  // slots of extended-range textures with a stand-in that a kernel may store
}

@end

@implementation SwitchMetalComputeEncoder

- (MTLDispatchType)dispatchType
{
    return _type;
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    SwitchMetalComputeState *next =
        [(id)state isKindOfClass:[SwitchMetalComputeState class]] ? (SwitchMetalComputeState *)state : nil;
    if (state && !next)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, 0,
                                 "a compute pipeline state of another device");
    if (_state != next) {
        _state = next;
        _kernelCurrent = false;
        for (uint32_t m = _storableStandIns; m; m &= m - 1)
            [self keyTexture:uint32_t(__builtin_ctz(m))];
    }
    setBinding(_cb, OpKernelState, 3, 0, state, 0);
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    recordBytes(_cb, cmd(OpKernelBytes, 3, uint16_t(index)), bytes, length);
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (buffer && ![(id)buffer isKindOfClass:[SwitchMetalBuffer class]])
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, index,
                                 "a kernel's buffer of another device");
    setBinding(_cb, OpKernelBuffer, 3, index, buffer, offset);
}

- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    Cmd m = cmd(OpKernelBufferOffset, 3, uint16_t(index));
    m.c = offset;
    [_cb record:m];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setBuffer:buffers[k] offset:offsets[k] atIndex:range.location + k];
}

static bool storableStandIn(SwitchMetalTexture *t)
{
    const gm20b::XrForm form = gm20b::xrForm(uint32_t(t->_descriptor.pixelFormat));
    return !heldInPlanes(t) && t->_raw.standIn && form != gm20b::XrForm::None && form != gm20b::XrForm::Planes &&
           (t->_descriptor.usage & MTLTextureUsageShaderWrite);
}

// The format a kernel of `state` is compiled to read `t` at `slot` in: its own where the kernel reads and writes its
// two planes apart (heldInPlanes, keyed :xr2p); else an RGB10A8_2P_XR10 texture with a stand-in, or an extended-range
// one the kernel does not store (made without ShaderWrite, or at a slot the state's reflection writes nothing to), as
// that stand-in, the RGBA16Float image of its values drawn from its codes as the encoder begins. With no state yet a
// slot is taken as stored, and keyed again when one is set. The two are never keyed together.
static MTLPixelFormat kernelFormat(SwitchMetalTexture *t, SwitchMetalComputeState *state, uint32_t slot)
{
    // a decoded format's texture is read as its stored image (decoded.h)
    if (const gm20b::DecodedFormat *f = gm20b::decodedFormat(uint32_t(t->_descriptor.pixelFormat)))
        return MTLPixelFormat(f->stored);
    if (heldInPlanes(t))
        return t->_descriptor.pixelFormat;
    const bool storable = storableStandIn(t) ? !state || (gm20bComputeTexturesWritten(state) >> slot & 1)
                                             : bool(t->_descriptor.usage & MTLTextureUsageShaderWrite);
    return MTLPixelFormat(gm20b::kernelTextureFormat(t->_raw, uint32_t(t->_descriptor.pixelFormat), storable));
}

static MTLTextureDescriptor *kernelDescriptor(SwitchMetalTexture *t, SwitchMetalComputeState *state, uint32_t slot)
{
    MTLTextureDescriptor *d = t->_descriptor;
    const MTLPixelFormat format = kernelFormat(t, state, slot);
    if (format == d.pixelFormat)
        return d;
    d = [d copy];
    d.pixelFormat = format;
    return d;
}

// Texture `index`'s item of the kernel key under the encoder's state
- (void)keyTexture:(uint32_t)index
{
    SwitchMetalTexture *t = _textures[index];
    gm20b::BoundTexture bound = {};
    if (t) {
        MTLTextureDescriptor *d = t->_descriptor;
        const MTLTextureSwizzleChannels w = d.swizzle;
        bound = {index,           kernelFormat(t, _state, index),
                 d.textureType,   d.mipmapLevelCount,
                 d.arrayLength,   {w.red, w.green, w.blue, w.alpha},
                 heldInPlanes(t), t->_gm20bOk ? t->_raw.addressMap : uint8_t(0)};
    }
    if (_specialization.bindTexture(index, t ? &bound : nullptr))
        _kernelCurrent = false;
}

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    SwitchMetalTexture *t =
        [(id)texture isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)texture : nil;
    if (texture && !t)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, index,
                                 "a kernel's texture of another device");
    if (index < 31 && _textures[index] != t) {
        _textures[index] = t;
        _storableStandIns = (_storableStandIns & ~(1u << index)) | uint32_t(t && storableStandIn(t)) << index;
        [self keyTexture:uint32_t(index)];
    }
    if (t)
        noteReadAsData(texture);
    setBinding(_cb, OpKernelTexture, 3, index, texture, 0);
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])textures withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setTexture:textures[k] atIndex:range.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    SwitchMetalSampler *s =
        [(id)sampler isKindOfClass:[SwitchMetalSampler class]] ? (SwitchMetalSampler *)sampler : nil;
    if (sampler && !s)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, index,
                                 "a kernel's sampler of another device");
    if (s && s->_raw.desc.border)
        _cb->_borderSamplers = true;
    if (index < 16 && _samplers[index] != s) {
        _samplers[index] = s;
        gm20b::BoundSampler bound = {};
        if (s) {
            MTLSamplerDescriptor *d = s->_descriptor;
            bound = {uint32_t(index),
                     d.minFilter,
                     d.magFilter,
                     d.mipFilter,
                     d.sAddressMode,
                     d.tAddressMode,
                     d.rAddressMode,
                     d.maxAnisotropy,
                     d.compareFunction,
                     d.borderColor,
                     d.lodMinClamp,
                     d.lodMaxClamp,
                     bool(d.normalizedCoordinates),
                     bool(d.lodAverage)};
        }
        if (_specialization.bindSampler(uint32_t(index), s ? &bound : nullptr))
            _kernelCurrent = false;
    }
    setBinding(_cb, OpKernelSampler, 3, index, sampler, 0);
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setSamplerState:samplers[k] atIndex:range.location + k];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (_specialization.bindMemory(uint32_t(index), uint32_t((length + 15) & ~NSUInteger(15))))
        _kernelCurrent = false;
    Cmd m = cmd(OpKernelMemory, 3, uint16_t(index));
    m.a = uint32_t(length);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    if (resource && ![(id)resource isKindOfClass:[SwitchMetalObject class]])
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, usage,
                                 "a resource used (useResource:) not made by this device");
    setBinding(_cb, OpKernelUse, 3, 0, resource, usage);
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
{
    for (NSUInteger k = 0; k < count; k++)
        [self useResource:resources[k] usage:usage];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
{
    (void)scope;
    [_cb record:cmd(OpKernelBarrier, 3)];
}

- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    [_cb record:cmd(OpKernelBarrier, 3)];
}

- (void)dispatch:(MTLSize)grid threads:(MTLSize)threads perThread:(bool)perThread
{
    [self dispatch:grid threads:threads perThread:perThread arguments:nil offset:0];
}

// `arguments`: dispatchThreadgroupsWithIndirectBuffer:'s, whose words the GPU reads as the grid; `grid` is then one
// threadgroup, the shape the kernel is looked up at
- (void)dispatch:(MTLSize)grid
         threads:(MTLSize)threads
       perThread:(bool)perThread
       arguments:(SwitchMetalBuffer *)arguments
          offset:(NSUInteger)offset
{
    if (!_state)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, 0,
                                 "a dispatch with no compute pipeline state set");
    const std::array<uint32_t, 3> t = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    const auto dispatch =
        gm20b::computeDispatch({grid.width, grid.height, grid.depth}, t, perThread, _state->_threadLimit);
    const auto &groups = dispatch.groups;
    // threads per threadgroup past the state's maxTotalThreadsPerThreadgroup (or 1024), or none on an axis: a dispatch
    // Metal's API validation stops, which the command buffer fails with
    const bool invalid = dispatch.reason == gm20b::ComputeDispatch::Edge || threads.width > 1024 ||
                         threads.height > 1024 || threads.depth > 1024;
    if (invalid)
        failCommandBuffer(_cb,
                          [NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu is not within the compute "
                                                     @"pipeline state's maxTotalThreadsPerThreadgroup %u",
                                                     (unsigned long)threads.width, (unsigned long)threads.height,
                                                     (unsigned long)threads.depth, _state->_threadLimit],
                          MTLCommandBufferErrorInvalidResource);
    // threads past 2^32 - 1 on an axis have no thread_position_in_grid (a uint3); a grid past the QMD's fields is
    // launched in slices (dispatch_bands.h kQmdGroups), past kMaxQmdSlices of them not at all
    const bool unaddressable = !invalid && (dispatch.reason != gm20b::ComputeDispatch::Ready ||
                                            gm20b::qmdSliceCount(groups) > gm20b::kMaxQmdSlices);
    if (unaddressable) {
        NSString *reason =
            [NSString stringWithFormat:@"A dispatch of %lux%lux%lu %@ of %lux%lux%lu threads is past what a thread's "
                                       @"position in the grid or raw's launches hold",
                                       (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth,
                                       perThread ? @"threads" : @"threadgroups", (unsigned long)threads.width,
                                       (unsigned long)threads.height, (unsigned long)threads.depth];
        failCommandBuffer(_cb, reason, MTLCommandBufferErrorInvalidResource);
    }
    const bool refused = invalid || unaddressable;
    bool sameShape = _shape.count == dispatch.count && _shape.multi == dispatch.multi;
    for (uint8_t i = 0; sameShape && i < dispatch.count; ++i)
        sameShape = _shape.regions[i].local == dispatch.regions[i].local;
    if (!sameShape)
        _kernelCurrent = false;
    std::shared_ptr<SwitchMetalKernelSet> kernels;
    if (!refused) {
        if (!_kernelCurrent || (_kernel && _kernel->retryable())) {
            _kernel = std::make_shared<SwitchMetalKernelSet>();
            _kernel->count = dispatch.count;
            for (uint8_t region = 0; region < dispatch.count; ++region) {
                auto specialization = _specialization;
                specialization.threads = dispatch.regions[region].local;
                specialization.regionMode = dispatch.multi;
                _kernel->jobs[region] = _state->_kernels.request(specialization, [&](auto pending) {
                    gm20b::Completion *completion = gRM->completion.get();
                    if (!completion) {
                        pending->publish({nullptr, true});
                        return;
                    }
                    const std::map<uint32_t, uint32_t> memory(specialization.memory.begin(),
                                                              specialization.memory.end());
                    auto ask = std::make_shared<gm20b::KernelDispatch>(specialization.threads.data(), memory,
                                                                       specialization.regionMode);
                    for (uint32_t slot = 0; slot < 31; ++slot)
                        if (_textures[slot])
                            ask->texture(slot, kernelDescriptor(_textures[slot], _state, slot),
                                         heldInPlanes(_textures[slot]));
                    for (uint32_t slot = 0; slot < 16; ++slot)
                        if (_samplers[slot])
                            ask->sampler(slot, _samplers[slot]->_descriptor);
                    SwitchMetalComputeState *state = _state;
                    completion->backgroundKernel([pending, ask, specialization, state, completion] {
                        SwitchMetalKernelJob::Run run{pending};
                        const auto text = specialization.text();
                        const auto key =
                            gm20b::computeKey(state->_air, state->_constants, state->_maxThreads,
                                              state->_multipleOfWidth, specialization.threads.data(), text.memory,
                                              text.textures, text.samplers, specialization.regionMode);
                        run.result.state = gm20b::computeState(*gRM->raw, gm20b::computeKeyHash(key));
                        if (run.result.state) {
                            run.result.retry = false;
                            return;
                        }
                        run.result.state = completion->kernel(state->_direct, ask->dispatch(), key, &run.result.retry,
                                                              state->_function, &run.result.gone);
                    });
                });
            }
            _shape = dispatch;
            _kernelCurrent = true;
        }
        kernels = _kernel;
    }
    const auto kernelState = kernels ? kernels->state() : SwitchMetalKernelSet::State::Refused;
    // a kernel no compiler made fails its command buffer at submission (computeReadiness), as one still being made waits;
    // one the compiler refused fails it as Metal's does a pipeline it could not compile
    const bool pending =
        kernelState == SwitchMetalKernelSet::State::Pending || kernelState == SwitchMetalKernelSet::State::Failed;
    const bool ready = kernelState == SwitchMetalKernelSet::State::Ready;
    if (!refused && !ready && !pending) {
        hit(_cb, PredComputeKernel, groups[0]);
        failCommandBuffer(_cb, @"a compute kernel the shader compiler refused", MTLCommandBufferErrorInternal,
                          PredComputeKernel);
    }
    Cmd m = cmd(OpDispatch, 3, perThread);
    m.c = uint64_t(t[0]) | uint64_t(t[1]) << 16 | uint64_t(t[2]) << 32;
    m.x = _cb->_extra.size();
    for (uint64_t v : {uint64_t(grid.width), uint64_t(grid.height), uint64_t(grid.depth), groups[0], groups[1],
                       groups[2], uint64_t(_stageIn.origin.x), uint64_t(_stageIn.origin.y), uint64_t(_stageIn.origin.z),
                       uint64_t(_stageIn.size.width), uint64_t(_stageIn.size.height), uint64_t(_stageIn.size.depth),
                       uint64_t(uintptr_t((__bridge void *)arguments)), uint64_t(offset),
                       uint64_t(uintptr_t((__bridge void *)_stageInBuffer)), uint64_t(_stageInOffset)})
        _cb->_extra.push_back(v);
    if (kernels) {
        _cb->_computeJobs.try_emplace(kernels.get(), kernels);
        if (pending)
            m.obj = kernels.get();
        if (ready)
            m.obj2 = kernels.get();
    }
    [_cb record:m];
}

- (BOOL)predicate:(gm20b::PredicateRecords::Kind)kind
           buffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
       comparison:(uint64_t)comparison
        reference:(uint32_t)reference
{
    _cb->_predicates.record(kind);
    const bool gm20bBuffer = [(id)buffer isKindOfClass:[SwitchMetalBuffer class]];
    const bool readsWord = kind == gm20b::PredicateRecords::StartIf || kind == gm20b::PredicateRecords::StartWhile ||
                           kind == gm20b::PredicateRecords::EndDoWhile;
    // a Start or an EndDoWhile naming no buffer withholds its compute unit's dispatches at the encode
    // (TegraGM20BMetalRangeStep.mm unitWithheld)
    const bool noBuffer = !buffer && readsWord;
    if (noBuffer)
        _predicateWithoutBuffer = true;
    const bool wordOutside =
        readsWord && !noBuffer && (!gm20bBuffer || (offset & ~3ull) + 4 > ((SwitchMetalBuffer *)buffer)->_raw.length);
    if (_cb->_predicates.misused())
        hit(_cb, PredComputePredicate, kind);
    else if (wordOutside)
        gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredComputePredicate, offset,
                          "a predicate word outside its buffer, or a buffer of another device");
    Cmd m = cmd(OpPredicate, uint8_t(kind));
    m.obj = wordOutside ? nullptr : (__bridge void *)buffer;
    m.c = offset & ~3ull;
    m.a = uint32_t(gm20b::predicateCompare(comparison));
    m.b = reference;
    [_cb record:m];
    return !_predicateWithoutBuffer;
}

- (void)encodeStartIf:(id<MTLBuffer>)buffer
               offset:(NSUInteger)offset
           comparison:(NSUInteger)comparison
       referenceValue:(uint32_t)reference
{
    [self predicate:gm20b::PredicateRecords::StartIf
             buffer:buffer
             offset:offset
         comparison:comparison
          reference:reference];
}

- (void)encodeStartElse
{
    [self predicate:gm20b::PredicateRecords::StartElse buffer:nil offset:0 comparison:0 reference:0];
}

- (BOOL)encodeEndIf
{
    return [self predicate:gm20b::PredicateRecords::EndIf buffer:nil offset:0 comparison:0 reference:0];
}

- (void)encodeStartWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference
{
    [self predicate:gm20b::PredicateRecords::StartWhile
             buffer:buffer
             offset:offset
         comparison:comparison
          reference:reference];
}

- (BOOL)encodeEndWhile
{
    return [self predicate:gm20b::PredicateRecords::EndWhile buffer:nil offset:0 comparison:0 reference:0];
}

- (void)encodeStartDoWhile
{
    [self predicate:gm20b::PredicateRecords::StartDoWhile buffer:nil offset:0 comparison:0 reference:0];
}

- (BOOL)encodeEndDoWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference
{
    return [self predicate:gm20b::PredicateRecords::EndDoWhile
                    buffer:buffer
                    offset:offset
                comparison:comparison
                 reference:reference];
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:groups threads:threads perThread:false];
}

- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:grid threads:threads perThread:true];
}

- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer
                          indirectBufferOffset:(NSUInteger)offset
                         threadsPerThreadgroup:(MTLSize)threads
{
    SwitchMetalBuffer *arguments =
        [(id)buffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)buffer : nil;
    if (!arguments)
        return gm20bComputeFails(_cb, gm20b::ComputeFailure::Invalid, PredCompute, offset,
                                 "indirect dispatch arguments of another device");
    // as Apple's GPU reads them (an M4 Max, API validation off: c38-audit/m2v-compute/probe): at the offset rounded
    // down to 4 bytes, a word past the buffer zero, so a grid whose last word lies past it launches nothing
    const NSUInteger aligned = offset & ~NSUInteger(3);
    if (aligned > arguments->_raw.length || arguments->_raw.length - aligned < 12)
        return skip(_cb, PredCompute, offset, arguments->_raw.length,
                    "indirect dispatch arguments past their buffer, whose grid reads a zero word: nothing launched");
    [_cb retain:buffer];
    [self dispatch:MTLSizeMake(1, 1, 1) threads:threads perThread:false arguments:arguments offset:aligned];
}

@end

@implementation SwitchMetalComputeEncoder (State)

- (SwitchMetalComputeState *)gm20bState
{
    return _state;
}

@end

void gm20bComputeFails(SwitchMetalCommandBuffer *cb, gm20b::ComputeFailure failure, SwitchMetalPred pred, uint64_t a,
                       const char *why)
{
    hit(cb, pred, a, 0, why);
    switch (failure) {
    case gm20b::ComputeFailure::Invalid:
        return failCommandBuffer(cb, [NSString stringWithFormat:@"%s: Metal's validation error", why],
                                 MTLCommandBufferErrorInvalidResource, pred);
    case gm20b::ComputeFailure::OutOfMemory:
        return failCommandBuffer(cb, @(why), MTLCommandBufferErrorOutOfMemory, pred);
    case gm20b::ComputeFailure::Internal:
        return failCommandBuffer(cb, @(why), MTLCommandBufferErrorInternal, pred);
    }
}

bool isKernelCommand(Op op)
{
    return (op >= OpKernelState && op <= OpDispatch) || op == OpPredicate || op == OpExecuteKernels;
}

// A binding command's attributeStride (TegraGM20BMetalStrides.mm: its x the stride + 1, 0 none)
static uint32_t commandStride(const Cmd &c)
{
    return c.x && c.x <= UINT32_MAX ? uint32_t(c.x - 1) : UINT32_MAX;
}

static bool encodePredicate(gm20b::ComputeEncoder &e, const Cmd &c, size_t at, const char **why)
{
    const uint64_t wordVa = c.obj ? ((__bridge SwitchMetalBuffer *)c.obj)->_raw.va + c.c : 0;
    const auto compare = gm20b::PredicateCompare(c.a);
    gm20b::Predication::Answer answer = gm20b::Predication::Answer::Misused;
    switch (c.stage) {
    case gm20b::PredicateRecords::StartIf:
        answer = e.startIf(wordVa, compare, c.b);
        break;
    case gm20b::PredicateRecords::StartElse:
        answer = e.startElse();
        break;
    case gm20b::PredicateRecords::EndIf:
        answer = e.endIf();
        break;
    case gm20b::PredicateRecords::StartWhile:
        answer = e.startWhile(wordVa, compare, c.b);
        break;
    case gm20b::PredicateRecords::EndWhile:
        answer = e.endWhile(uint32_t(at));
        break;
    case gm20b::PredicateRecords::StartDoWhile:
        answer = e.startDoWhile();
        break;
    case gm20b::PredicateRecords::EndDoWhile:
        answer = e.endDoWhile(wordVa, compare, c.b, uint32_t(at));
        break;
    default:
        break;
    }
    if (answer != gm20b::Predication::Answer::Done)
        *why = c.stage >= gm20b::PredicateRecords::StartWhile
                   ? "a predicated loop the driver cannot lay down (deeper than eight, a body of more than 32 push "
                     "segments, one its unit ended inside, or more GP entries than one nest takes)"
                   : "a predication record the driver cannot run";
    return answer == gm20b::Predication::Answer::Done;
}

bool encodeKernel(gm20b::ComputeEncoder &e, gm20b::CommandBuffer &rc, SwitchMetalCommandBuffer *cb, const Cmd &c,
                  size_t at, const char **why)
{
    id object = (__bridge id)c.obj;
    if (c.op == OpKernelBytes || c.op == OpKernelBuffer || c.op == OpKernelBufferOffset)
        e.setBufferStride(c.index, commandStride(c));
    switch (c.op) {
    case OpKernelBytes: {
        const gm20b::Buffer &k = cb->_bytes.chunks[c.c];
        if (!k.wc)
            rc.clean(k.cpu + c.a, std::max<uint32_t>((c.b + 255) & ~255u, 256));
        e.setBytesAt(c.index, k.va + c.a, c.b, &k);
        break;
    }
    case OpKernelBuffer: {
        SwitchMetalBuffer *b = (SwitchMetalBuffer *)object;
        e.setBuffer(c.index, b ? &b->_raw : nullptr, c.c);
        break;
    }
    case OpKernelBufferOffset:
        e.setBufferOffset(c.index, c.c);
        break;
    case OpKernelClockAnchors:
        e.setKernelBytes(c.index, c.c, c.b);
        break;
    case OpKernelTexture: {
        SwitchMetalTexture *t = (SwitchMetalTexture *)object;
        e.setTexture(c.index, t ? &t->_raw : nullptr);
        break;
    }
    case OpKernelSampler:
        e.setSampler(c.index, object ? &((SwitchMetalSampler *)object)->_raw : nullptr);
        break;
    case OpKernelBarrier:
        e.barrier();
        break;
    case OpPredicate:
        return encodePredicate(e, c, at, why);
    case OpDispatch: {
        const uint32_t groups[3] = {uint32_t(extra(cb, c.x + 3)), uint32_t(extra(cb, c.x + 4)),
                                    uint32_t(extra(cb, c.x + 5))},
                       threads[3] = {uint32_t(c.c & 0xffff), uint32_t(c.c >> 16) & 0xffff, uint32_t(c.c >> 32)};
        e.setState(static_cast<const gm20b::ComputeState *>(c.obj2));
        e.setStageInRegion({uint32_t(extra(cb, c.x + 6)), uint32_t(extra(cb, c.x + 7)), uint32_t(extra(cb, c.x + 8)),
                            uint32_t(extra(cb, c.x + 9)), uint32_t(extra(cb, c.x + 10)),
                            uint32_t(extra(cb, c.x + 11))});
        return e.dispatch(groups, threads, why);
    }
    case OpExecuteKernels:
        return encodeKernelCommands(e, rc, (SwitchMetalKernelExecute *)object, why);
    default:  // the state and threadgroup memory are the dispatch's kernel's; residency is every buffer's already
        break;
    }
    return true;
}
