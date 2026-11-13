// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "RawComplete.h"
#include "RawDirect.h"
#include "raw_compute.h"
#include "raw_compute_key.h"
#include "raw_compute_lookup.h"
#import "RawMetalComputeState.h"
#import "RawMetalResolveAsk.h"
#include "raw_predication.h"
#import "RawMetalCounters.h"
#include "TegraGM20BClockABI.h"
#import "RawMetalIndirectCompute.h"
#include "raw_view.h"
#include "raw_surface_view.h"

using RMKernelJob = raw::ComputeKernelJob<raw::ComputeState>;

@interface RMComputeEncoder () {
    RMComputeState *_state;
    raw::ComputeSpecialization _specialization;
    std::shared_ptr<RMKernelSet> _kernel;
    raw::ComputeDispatch _shape;
    bool _kernelCurrent;
    // what the dispatches' kernels may read (unretained: the command buffer retains them): the textures, and the buffers'
    // addresses (0: setBytes:, which the raw path uploads aligned)
    RMTexture *_textures[31];
    RMSampler *_samplers[16];
}

@end

@implementation RMComputeEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputeEncoder");
}

- (MTLDispatchType)dispatchType
{
    return _type;
}

- (void)refuse:(RMPred)pred value:(uint64_t)a why:(NSString *)why
{
    [_cb takeReason:WhyCompute why:why];
    hit(_cb, pred, a);
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    RMComputeState *next = [(id)state isKindOfClass:[RMComputeState class]] ? (RMComputeState *)state : nil;
    if (_state != next) {
        _state = next;
        _kernelCurrent = false;
    }
    setBinding(_cb, OpKernelState, 3, 0, state, 0);
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    recordBytes(_cb, cmd(OpKernelBytes, 3, uint16_t(index)), bytes, length);
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (buffer && ![(id)buffer isKindOfClass:[RMBuffer class]])
        [self refuse:PredCompute value:index why:@"a kernel's buffer not made by this device"];
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

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    RMTexture *t = [(id)texture isKindOfClass:[RMTexture class]] ? (RMTexture *)texture : nil;
    if (index < 31 && _textures[index] != t) {
        _textures[index] = t;
        raw::BoundTexture bound = {};
        if (t) {
            MTLTextureDescriptor *d = t->_descriptor;
            const MTLTextureSwizzleChannels w = d.swizzle;
            bound = {uint32_t(index),    d.pixelFormat, d.textureType,
                     d.mipmapLevelCount, d.arrayLength, {w.red, w.green, w.blue, w.alpha}};
        }
        if (_specialization.bindTexture(uint32_t(index), t ? &bound : nullptr))
            _kernelCurrent = false;
    }
    if (texture && !t)
        [self refuse:PredCompute value:index why:@"a kernel's texture not made by this device"];
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
    if (sampler && ![(id)sampler isKindOfClass:[RMSampler class]])
        [self refuse:PredCompute value:index why:@"a kernel's sampler not made by this device"];
    RMSampler *s = [(id)sampler isKindOfClass:[RMSampler class]] ? (RMSampler *)sampler : nil;
    if (index < 16 && _samplers[index] != s) {
        _samplers[index] = s;
        raw::BoundSampler bound = {};
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
    const std::array<uint32_t, 3> t = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    const auto dispatch =
        raw::computeDispatch({grid.width, grid.height, grid.depth}, t, perThread, _state ? _state->_threadLimit : 1024);
    const auto &groups = dispatch.groups;
    const bool refused = dispatch.reason != raw::ComputeDispatch::Ready || threads.width > 1024 ||
                         threads.height > 1024 || threads.depth > 1024;
    if (refused)
        [self refuse:PredComputeKernel value:groups[0] why:@"a dispatch outside the raw grid or local-size limits"];
    bool sameShape = _shape.count == dispatch.count && _shape.multi == dispatch.multi;
    for (uint8_t i = 0; sameShape && i < dispatch.count; ++i)
        sameShape = _shape.regions[i].local == dispatch.regions[i].local;
    if (!sameShape)
        _kernelCurrent = false;
    std::shared_ptr<RMKernelSet> kernels;
    if (!refused && _state && _state->_keyed) {
        if (!_kernelCurrent || (_kernel && _kernel->retryable())) {
            _kernel = std::make_shared<RMKernelSet>();
            _kernel->count = dispatch.count;
            for (uint8_t region = 0; region < dispatch.count; ++region) {
                auto specialization = _specialization;
                specialization.threads = dispatch.regions[region].local;
                specialization.regionMode = dispatch.multi;
                _kernel->jobs[region] = _state->_kernels.request(specialization, [&](auto pending) {
                    raw::Completion *completion = gRM->completion.get();
                    if (!completion) {
                        pending->publish({nullptr, true});
                        return;
                    }
                    const std::map<uint32_t, uint32_t> memory(specialization.memory.begin(),
                                                              specialization.memory.end());
                    auto ask = std::make_shared<raw::KernelDispatch>(specialization.threads.data(), memory,
                                                                     specialization.regionMode);
                    for (uint32_t slot = 0; slot < 31; ++slot)
                        if (_textures[slot])
                            ask->texture(slot, _textures[slot]->_descriptor);
                    for (uint32_t slot = 0; slot < 16; ++slot)
                        if (_samplers[slot])
                            ask->sampler(slot, _samplers[slot]->_descriptor);
                    RMComputeState *state = _state;
                    completion->backgroundKernel([pending, ask, specialization, state, completion, grid, threads,
                                                  perThread, region] {
                        RMKernelJob::Run run{pending};
                        const auto text = specialization.text();
                        const auto key =
                            raw::computeKey(state->_air, state->_constants, state->_maxThreads, state->_multipleOfWidth,
                                            specialization.threads.data(), text.memory, text.textures, text.samplers,
                                            specialization.regionMode);
                        run.result.state = raw::computeState(*gRM->raw, raw::computeKeyHash(key));
                        if (run.result.state) {
                            run.result.retry = false;
                            return;
                        }
                        uint64_t retryAtUs = 0;
                        run.result.state = completion->kernel(state->_direct, ask->dispatch(), key, &run.result.retry,
                                                              &retryAtUs, state->_function, &run.result.gone);
                        if (run.result.retry && retryAtUs) {
                            run.result.deferredRetry = true;
                            const uint64_t now = nowUs(), delay = retryAtUs > now ? retryAtUs - now : 0;
                            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay * NSEC_PER_USEC)),
                                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                                               pending->allowRetry();
                                           });
                        }
                        // the journal names m2v's state: a state the pack answered has none
                        if (run.result.state && !region && state->_metadata)
                            native_bridge_metal_journal_dispatch(state->_metadata, grid, threads, perThread,
                                                                 text.memory.c_str(), text.textures.c_str(),
                                                                 text.samplers.c_str());
                    });
                });
            }
            _shape = dispatch;
            _kernelCurrent = true;
        }
        kernels = _kernel;
    }
    const auto kernelState = kernels ? kernels->state() : RMKernelSet::State::Refused;
    // a kernel no compiler made fails its command buffer at submission (computeReadiness), as one still being made waits
    const bool pending = kernelState == RMKernelSet::State::Pending || kernelState == RMKernelSet::State::Failed;
    const bool ready = kernelState == RMKernelSet::State::Ready;
    if (!refused && !ready && !pending)
        [self refuse:PredComputeKernel value:groups[0] why:@"a dispatch the raw path has no kernel for"];
    Cmd m = cmd(OpDispatch, 3, perThread);
    m.c = uint64_t(t[0]) | uint64_t(t[1]) << 16 | uint64_t(t[2]) << 32;
    m.x = _cb->_extra.size();
    for (uint64_t v : {uint64_t(grid.width), uint64_t(grid.height), uint64_t(grid.depth), groups[0], groups[1],
                       groups[2], uint64_t(_stageIn.origin.x), uint64_t(_stageIn.origin.y), uint64_t(_stageIn.origin.z),
                       uint64_t(_stageIn.size.width), uint64_t(_stageIn.size.height), uint64_t(_stageIn.size.depth)})
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

- (BOOL)predicate:(raw::PredicateRecords::Kind)kind
           buffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
       comparison:(uint64_t)comparison
        reference:(uint32_t)reference
{
    _cb->_predicates.record(kind);
    const bool rawBuffer = [(id)buffer isKindOfClass:[RMBuffer class]];
    const bool readsWord = kind == raw::PredicateRecords::StartIf || kind == raw::PredicateRecords::StartWhile ||
                           kind == raw::PredicateRecords::EndDoWhile;
    if (!buffer && (kind == raw::PredicateRecords::StartIf || kind == raw::PredicateRecords::StartWhile))
        _predicateWithoutBuffer = true;
    if (_cb->_predicates.misused())
        hit(_cb, PredComputePredicate, kind);
    else if (readsWord && (!rawBuffer || (offset & ~3ull) + 4 > ((RMBuffer *)buffer)->_raw.length))
        [self refuse:PredComputePredicate value:offset why:@"a predicate word outside a raw buffer"];
    Cmd m = cmd(OpPredicate, uint8_t(kind));
    m.obj = (__bridge void *)buffer;
    m.c = offset & ~3ull;
    m.a = uint32_t(raw::predicateCompare(comparison));
    m.b = reference;
    [_cb record:m];
    return !_predicateWithoutBuffer;
}

- (void)encodeStartIf:(id<MTLBuffer>)buffer
               offset:(NSUInteger)offset
           comparison:(NSUInteger)comparison
       referenceValue:(uint32_t)reference
{
    [self predicate:raw::PredicateRecords::StartIf
             buffer:buffer
             offset:offset
         comparison:comparison
          reference:reference];
}

- (void)encodeStartElse
{
    [self predicate:raw::PredicateRecords::StartElse buffer:nil offset:0 comparison:0 reference:0];
}

- (BOOL)encodeEndIf
{
    return [self predicate:raw::PredicateRecords::EndIf buffer:nil offset:0 comparison:0 reference:0];
}

- (void)encodeStartWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference
{
    [self predicate:raw::PredicateRecords::StartWhile
             buffer:buffer
             offset:offset
         comparison:comparison
          reference:reference];
}

- (BOOL)encodeEndWhile
{
    return [self predicate:raw::PredicateRecords::EndWhile buffer:nil offset:0 comparison:0 reference:0];
}

- (void)encodeStartDoWhile
{
    [self predicate:raw::PredicateRecords::StartDoWhile buffer:nil offset:0 comparison:0 reference:0];
}

- (BOOL)encodeEndDoWhile:(id<MTLBuffer>)buffer
                  offset:(NSUInteger)offset
              comparison:(NSUInteger)comparison
          referenceValue:(uint32_t)reference
{
    return [self predicate:raw::PredicateRecords::EndDoWhile
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

@end

@implementation RMComputeEncoder (State)

- (RMComputeState *)rmState
{
    return _state;
}

@end

bool isKernelCommand(Op op)
{
    return (op >= OpKernelState && op <= OpDispatch) || op == OpPredicate || op == OpExecuteKernels;
}

// A binding command's attributeStride (RawMetalStrides.mm: its x the stride + 1, 0 none)
static uint32_t commandStride(const Cmd &c)
{
    return c.x && c.x <= UINT32_MAX ? uint32_t(c.x - 1) : UINT32_MAX;
}

void replayKernel(id encoder, RMCommandBuffer *cb, const Cmd &c, std::set<RMBuffer *> &buffers)
{
    id<MTLComputeCommandEncoder> e = encoder;
    id object = (__bridge id)c.obj;
    switch (c.op) {
    case OpKernelState:
        [e setComputePipelineState:twinOf(object)];
        break;
    case OpKernelBytes:
        if (c.x)
            [e setBytes:cb->_bytes.chunks[c.c].cpu + c.a length:c.b attributeStride:commandStride(c) atIndex:c.index];
        else
            [e setBytes:cb->_bytes.chunks[c.c].cpu + c.a length:c.b atIndex:c.index];
        break;
    case OpKernelBuffer:
        if ([object isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)object);
        if (c.x)
            [e setBuffer:twinOf(object) offset:c.c attributeStride:commandStride(c) atIndex:c.index];
        else
            [e setBuffer:twinOf(object) offset:c.c atIndex:c.index];
        break;
    case OpKernelBufferOffset:
        if (c.x)
            [e setBufferOffset:c.c attributeStride:commandStride(c) atIndex:c.index];
        else
            [e setBufferOffset:c.c atIndex:c.index];
        break;
    case OpKernelClockAnchors: {
        const TegraGM20BAbiClockAnchors anchors = rmClockAnchorsNow();
        [e setBytes:&anchors length:sizeof anchors atIndex:c.index];
        break;
    }
    case OpKernelTexture:
        [e setTexture:twinOf(object) atIndex:c.index];
        break;
    case OpKernelSampler:
        [e setSamplerState:twinOf(object) atIndex:c.index];
        break;
    case OpKernelMemory:
        [e setThreadgroupMemoryLength:c.a atIndex:c.index];
        break;
    case OpKernelUse:
        if ([object isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)object);
        if (![object isKindOfClass:[RMHeap class]])
            [e useResource:twinOf(object) usage:MTLResourceUsage(c.c)];
        break;
    case OpKernelBarrier:
        [e memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        break;
    case OpDispatch: {
        const MTLSize grid = MTLSizeMake(extra(cb, c.x), extra(cb, c.x + 1), extra(cb, c.x + 2)),
                      threads = MTLSizeMake(c.c & 0xffff, (c.c >> 16) & 0xffff, c.c >> 32);
        [e setStageInRegion:MTLRegionMake3D(extra(cb, c.x + 6), extra(cb, c.x + 7), extra(cb, c.x + 8),
                                            extra(cb, c.x + 9), extra(cb, c.x + 10), extra(cb, c.x + 11))];
        if (c.index)
            [e dispatchThreads:grid threadsPerThreadgroup:threads];
        else
            [e dispatchThreadgroups:grid threadsPerThreadgroup:threads];
        break;
    }
    case OpExecuteKernels:
        replayKernelCommands(e, (__bridge RMKernelExecute *)c.obj, buffers);
        break;
    default:
        break;
    }
}

static bool encodePredicate(raw::ComputeEncoder &e, const Cmd &c, size_t at, const char **why)
{
    const uint64_t wordVa = c.obj ? ((__bridge RMBuffer *)c.obj)->_raw.va + c.c : 0;
    const auto compare = raw::PredicateCompare(c.a);
    raw::Predication::Answer answer = raw::Predication::Answer::Misused;
    switch (c.stage) {
    case raw::PredicateRecords::StartIf:
        answer = e.startIf(wordVa, compare, c.b);
        break;
    case raw::PredicateRecords::StartElse:
        answer = e.startElse();
        break;
    case raw::PredicateRecords::EndIf:
        answer = e.endIf();
        break;
    case raw::PredicateRecords::StartWhile:
        answer = e.startWhile(wordVa, compare, c.b);
        break;
    case raw::PredicateRecords::EndWhile:
        answer = e.endWhile(uint32_t(at));
        break;
    case raw::PredicateRecords::StartDoWhile:
        answer = e.startDoWhile();
        break;
    case raw::PredicateRecords::EndDoWhile:
        answer = e.endDoWhile(wordVa, compare, c.b, uint32_t(at));
        break;
    default:
        break;
    }
    if (answer != raw::Predication::Answer::Done)
        *why = c.stage >= raw::PredicateRecords::StartWhile
                   ? "a predicated loop the raw path cannot lay down (deeper than eight, a body of more than 32 push "
                     "segments, one its unit ended inside, or more GP entries than one nest takes)"
                   : "a predication record the raw path cannot run";
    return answer == raw::Predication::Answer::Done;
}

bool encodeKernel(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMCommandBuffer *cb, const Cmd &c, size_t at,
                  const char **why)
{
    id object = (__bridge id)c.obj;
    if (c.op == OpKernelBytes || c.op == OpKernelBuffer || c.op == OpKernelBufferOffset)
        e.setBufferStride(c.index, commandStride(c));
    switch (c.op) {
    case OpKernelBytes: {
        const raw::Buffer &k = cb->_bytes.chunks[c.c];
        if (!k.wc)
            rc.clean(k.cpu + c.a, std::max<uint32_t>((c.b + 255) & ~255u, 256));
        e.setBytesAt(c.index, k.va + c.a, c.b, &k);
        break;
    }
    case OpKernelBuffer: {
        RMBuffer *b = (RMBuffer *)object;
        e.setBuffer(c.index, b ? &b->_raw : nullptr, c.c);
        break;
    }
    case OpKernelBufferOffset:
        e.setBufferOffset(c.index, c.c);
        break;
    case OpKernelClockAnchors:
        e.setBytesAt(c.index, c.c, c.b);
        break;
    case OpKernelTexture: {
        RMTexture *t = (RMTexture *)object;
        e.setTexture(c.index, t ? &t->_raw : nullptr);
        break;
    }
    case OpKernelSampler:
        e.setSampler(c.index, object ? &((RMSampler *)object)->_raw : nullptr);
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
        e.setState(static_cast<const raw::ComputeState *>(c.obj2));
        e.setStageInRegion({uint32_t(extra(cb, c.x + 6)), uint32_t(extra(cb, c.x + 7)), uint32_t(extra(cb, c.x + 8)),
                            uint32_t(extra(cb, c.x + 9)), uint32_t(extra(cb, c.x + 10)),
                            uint32_t(extra(cb, c.x + 11))});
        return e.dispatch(groups, threads, why);
    }
    case OpExecuteKernels:
        return encodeKernelCommands(e, rc, (RMKernelExecute *)object, why);
    default:  // the state and threadgroup memory are the dispatch's kernel's; residency is every buffer's already
        break;
    }
    return true;
}
