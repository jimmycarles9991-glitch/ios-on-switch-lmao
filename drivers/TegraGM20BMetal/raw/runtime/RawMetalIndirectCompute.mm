// SPDX-License-Identifier: MIT
// MTLIndirectCommandBuffer of dispatches (raw_icb_compute.h): a command's QMDs, root tables and set 0 are written into the
// buffer's own GPU memory as the CPU encodes it, with its own pipeline's kernels made then, and its slot's words launch them.
// A buffer that inherits the encoder's buffers also writes each command's bind slot, from which an execute's GPU writes its
// descriptors; one that inherits the encoder's pipeline keeps its commands as the CPU encoded them and writes them all for
// a pipeline at the first execute under it after a change (rmPipelineVariant).
#include "raw_lock.h"
#import "RawMetalIndirectCompute.h"
#include "RawDirect.h"
#include "raw_compute_key.h"

using RMKernelJob = raw::ComputeKernelJob<raw::ComputeState>;

@interface RMIndirectComputeBuffer ()
- (void)write:(NSUInteger)index;
@end

namespace {

uint32_t *computeSlot(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * raw::icb::kComputeSlotWords;
}

void waitFor(RMKernelJob &job)
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    if (job.observe([done] {
            dispatch_semaphore_signal(done);
        }) == RMKernelJob::State::Pending)
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

std::shared_ptr<RMKernelJob> requestKernel(RMComputeState *state, const raw::ComputeSpecialization &specialization)
{
    return state->_kernels.request(specialization, [&](std::shared_ptr<RMKernelJob> pending) {
        RMKernelJob::Run run{pending};
        raw::Completion *completion = gRM->completion.get();
        if (!completion)
            return;
        const std::map<uint32_t, uint32_t> memory(specialization.memory.begin(), specialization.memory.end());
        raw::KernelDispatch ask(specialization.threads.data(), memory, specialization.regionMode);
        const auto text = specialization.text();
        const auto key = raw::computeKey(state->_air, state->_constants, state->_maxThreads, state->_multipleOfWidth,
                                         specialization.threads.data(), text.memory, text.textures, text.samplers,
                                         specialization.regionMode);
        run.result.state = raw::computeState(*gRM->raw, raw::computeKeyHash(key));
        if (run.result.state) {
            run.result.retry = false;
            return;
        }
        run.result.state =
            completion->kernel(state->_direct, ask.dispatch(), key, &run.result.retry, nullptr, state->_function);
    });
}

// A block of `bytes` in `chunks`, a chunk added where the last has no room
std::shared_ptr<RMICBBlock> blockIn(std::vector<raw::Buffer> &chunks, uint64_t &used, uint32_t bytes)
{
    constexpr uint64_t kChunk = 256 << 10;
    if (chunks.empty() || used + bytes > chunks.back().length) {
        raw::Buffer chunk;
        if (!gRM->raw->createBuffer(std::max<uint64_t>(kChunk, bytes), chunk, true))
            return nullptr;
        chunks.push_back(chunk);
        used = 0;
    }
    const raw::Buffer &chunk = chunks.back();
    auto block = std::make_shared<RMICBBlock>(RMICBBlock{chunk.cpu + used, chunk.va + used, bytes});
    used += (bytes + raw::icb::kComputeBlockAlign - 1) & ~uint64_t(raw::icb::kComputeBlockAlign - 1);
    return block;
}

std::shared_ptr<RMICBBlock> newBlock(RMIndirectComputeBuffer *b, uint32_t bytes)
{
    return blockIn(b->_chunks, b->_chunkUsed, bytes);
}

}  // namespace

std::shared_ptr<RMKernelSet> rmCommandKernels(RMComputeState *pipeline, const RMICBDispatch &c,
                                              raw::ComputeDispatch &shape)
{
    if (!pipeline || !pipeline->_keyed || !c.dispatched)
        return nullptr;
    shape = raw::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
    if (shape.reason != raw::ComputeDispatch::Ready || !shape.count)
        return nullptr;
    auto kernels = std::make_shared<RMKernelSet>();
    kernels->count = shape.count;
    for (uint8_t region = 0; region < shape.count; ++region) {
        raw::ComputeSpecialization specialization;
        specialization.threads = shape.regions[region].local;
        specialization.regionMode = shape.multi;
        specialization.memory = c.memory;
        kernels->jobs[region] = requestKernel(pipeline, specialization);
        waitFor(*kernels->jobs[region]);
    }
    return kernels->state() == RMKernelSet::State::Ready ? kernels : nullptr;
}

uint32_t rmCommandBytes(const RMKernelSet &kernels)
{
    uint32_t bytes = 0;
    for (uint8_t region = 0; region < kernels.count; ++region)
        bytes += raw::icb::computeRegionBytes(kernels.jobs[region]->value()->rec->setBytes);
    return bytes;
}

void rmCommandBuffers(const RMICBDispatch &c, raw::ComputeEncoder::IcbBuffer (&out)[31])
{
    for (uint32_t slot = 0; slot < 31; slot++) {
        RMBuffer *b = c.buffers[slot];
        out[slot] = {};
        if (b && b->_raw.va && c.offsets[slot] <= b->_raw.length)
            out[slot] = {b->_raw.va + c.offsets[slot],
                         uint32_t(std::min<uint64_t>(b->_raw.length - c.offsets[slot], 0xffffffffu))};
    }
}

bool rmCommandBinds(const RMKernelSet &kernels, const RMICBBlock &block, RMICBDispatch &c, const char **why)
{
    c.binds.clear();
    c.strideWords.clear();
    c.bindSlots = c.alignedSlots = c.strideSlots = 0;
    uint32_t at = 0;
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const raw::ComputeState &k = *kernels.jobs[region]->value();
        if (!raw::ComputeEncoder::icbBinds(k, block.va + at, c.binds, c.strideWords, why))
            return false;
        at += raw::icb::computeRegionBytes(k.rec->setBytes);
    }
    if (!raw::icb::bindsFit(uint32_t(c.binds.size()), uint32_t(c.strideWords.size()))) {
        *why = "more inherited buffer descriptors and strides than a bind slot holds";
        return false;
    }
    for (const raw::icb::ComputeBind &b : c.binds) {
        c.bindSlots |= 1u << b.slot;
        if (!b.residual)
            c.alignedSlots |= 1u << b.slot;
    }
    for (const raw::icb::ComputeStride &s : c.strideWords)
        c.strideSlots |= 1u << s.slot;
    return true;
}

uint32_t *rmBindSlot(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_binds.cpu) + index * raw::icb::kComputeBindSlotWords;
}

RMICBVariant::~RMICBVariant()
{
    for (raw::Buffer &chunk : chunks)
        gRM->raw->destroyBuffer(chunk);
    if (slots.va)
        gRM->raw->destroyBuffer(slots);
    if (binds.va)
        gRM->raw->destroyBuffer(binds);
}

std::shared_ptr<RMICBVariant> rmPipelineVariant(RMIndirectComputeBuffer *b, RMComputeState *pipeline,
                                                const std::vector<std::shared_ptr<RMKernelSet>> &kernels,
                                                const char **why)
{
    auto v = std::make_shared<RMICBVariant>();
    v->generation = b->_generation;
    v->missing.assign(b->_count, 0);
    const uint64_t count = b->_count;
    if (!gRM->raw->createBuffer(count * raw::icb::kComputeSlotWords * 4, v->slots, true) ||
        (b->_inheritBuffers && !gRM->raw->createBuffer(count * raw::icb::kComputeBindSlotWords * 4, v->binds, true))) {
        *why = "no memory for an indirect command buffer's commands at the encoder's pipeline";
        return nullptr;
    }
    for (NSUInteger i = 0; i < count; i++) {
        uint32_t *slot = reinterpret_cast<uint32_t *>(v->slots.cpu) + i * raw::icb::kComputeSlotWords;
        uint32_t *bind = b->_inheritBuffers
                             ? reinterpret_cast<uint32_t *>(v->binds.cpu) + i * raw::icb::kComputeBindSlotWords
                             : nullptr;
        raw::icb::computeEmpty(slot);
        if (bind)
            raw::icb::bindsEmpty(bind);
        const RMICBDispatch &c = b->_dispatches[i];
        const RMKernelSet *k = i < kernels.size() ? kernels[i].get() : nullptr;
        if (!c.dispatched)
            continue;
        if (!k) {
            v->missing[i] = 1;
            continue;
        }
        const raw::ComputeDispatch shape = raw::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
        const std::shared_ptr<RMICBBlock> block = blockIn(v->chunks, v->chunkUsed, rmCommandBytes(*k));
        raw::ComputeEncoder::IcbBuffer buffers[31];
        if (!b->_inheritBuffers)
            rmCommandBuffers(c, buffers);
        RMICBDispatch bound;
        raw::icb::ComputeCommand words{};
        if (!block || !rmCommandRegions(*k, shape, c, buffers, c.strides, *block, words, why) ||
            (bind && !rmCommandBinds(*k, *block, bound, why)))
            return nullptr;
        words.barrier = c.barrier;
        raw::icb::dispatch(slot, uint32_t(i), words);
        if (bind) {
            raw::icb::binds(bind, bound.binds.data(), uint32_t(bound.binds.size()), bound.strideWords.data(),
                            uint32_t(bound.strideWords.size()));
            v->bindSlots |= bound.bindSlots;
            v->alignedSlots |= bound.alignedSlots;
            v->strideSlots |= bound.strideSlots;
        }
    }
    return v;
}

bool rmCommandRegions(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBDispatch &c,
                      const raw::ComputeEncoder::IcbBuffer (&buffers)[31], const std::array<uint32_t, 31> &strides,
                      const RMICBBlock &block, raw::icb::ComputeCommand &words, const char **why)
{
    raw::StageIn stageIn;
    stageIn.strides = strides;
    if (c.stageIn)
        stageIn.setRegion({c.stageInRegion.origin.x, c.stageInRegion.origin.y, c.stageInRegion.origin.z,
                           c.stageInRegion.size.width, c.stageInRegion.size.height, c.stageInRegion.size.depth});
    uint32_t at = 0;
    words.regions = kernels.count;
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const raw::ComputeState &k = *kernels.jobs[region]->value();
        if (!raw::ComputeEncoder::icbRegion(*gRM->raw, k, shape.regions[region], shape.grid, shape.nominal, buffers,
                                            stageIn, block.cpu + at, block.va + at, why))
            return false;
        words.qmd[region] = block.va + at;
        at += raw::icb::computeRegionBytes(k.rec->setBytes);
    }
    return true;
}

@implementation RMIndirectComputeBuffer

- (void)dealloc
{
    for (raw::Buffer &chunk : _chunks)
        gRM->raw->destroyBuffer(chunk);
    if (_binds.va)
        gRM->raw->destroyBuffer(_binds);
}

// Command `index`'s words, from what the CPU encoded: its regions written now where it inherits nothing
- (void)write:(NSUInteger)index
{
    RMICBDispatch &c = _dispatches[index];
    c.raw = false;
    c.binds.clear();
    c.strideWords.clear();
    c.bindSlots = c.alignedSlots = c.strideSlots = 0;
    _bindsChanged = true;
    _generation++;
    if (_inheritBuffers)
        raw::icb::bindsEmpty(rmBindSlot(self, index));
    raw::ComputeDispatch shape;
    if (!_inheritPipeline)
        c.kernels = rmCommandKernels(c.pipeline, c, shape);
    if (_inheritPipeline || !c.kernels) {
        if (c.dispatched && !_inheritPipeline)
            SWITCH_REFUSE("raw", "icb.kernel", "indirect dispatch command %lu: no raw kernel at its shape",
                          (unsigned long)index);
        raw::icb::computeEmpty(computeSlot(self, index));
        return;
    }
    const uint32_t bytes = rmCommandBytes(*c.kernels);
    if (!c.block || c.block.use_count() > 1 || c.block->bytes < bytes)
        c.block = newBlock(self, bytes);
    raw::ComputeEncoder::IcbBuffer buffers[31];
    if (!_inheritBuffers)
        rmCommandBuffers(c, buffers);
    const char *why = nullptr;
    if (!c.block || !rmCommandRegions(*c.kernels, shape, c, buffers, c.strides, *c.block, c.words, &why) ||
        (_inheritBuffers && !rmCommandBinds(*c.kernels, *c.block, c, &why))) {
        SWITCH_REFUSE_DETAIL("raw", "icb.region", uintptr_t(why), "indirect dispatch command %lu: %s",
                             (unsigned long)index, why ? why : "no memory for its regions");
        raw::icb::computeEmpty(computeSlot(self, index));
        return;
    }
    c.words.barrier = c.barrier;
    raw::icb::dispatch(computeSlot(self, index), uint32_t(index), c.words);
    if (_inheritBuffers)
        raw::icb::binds(rmBindSlot(self, index), c.binds.data(), uint32_t(c.binds.size()), c.strideWords.data(),
                        uint32_t(c.strideWords.size()));
    c.raw = true;
}

- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)index
{
    (void)index;
    return nil;
}

- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)index
{
    if (index >= _count)
        return nil;
    RMIndirectComputeCommand *c = [RMIndirectComputeCommand new];
    c->_buffer = self;
    c->_index = index;
    return (id<MTLIndirectComputeCommand>)c;
}

- (void)resetWithRange:(NSRange)range
{
    std::lock_guard<raw::Mutex> hold(_lock);
    for (NSUInteger i = range.location; i < std::min<NSUInteger>(NSMaxRange(range), _count); i++) {
        _dispatches[i] = {};
        raw::icb::computeEmpty(computeSlot(self, i));
        if (_inheritBuffers)
            raw::icb::bindsEmpty(rmBindSlot(self, i));
    }
    _bindsChanged = true;
    _generation++;
}

@end

@implementation RMIndirectComputeCommand

- (void)edit:(void (^)(RMICBDispatch &c))change
{
    std::lock_guard<raw::Mutex> hold(_buffer->_lock);
    change(_buffer->_dispatches[_index]);
    [_buffer write:_index];
}

// A barrier changes the slot's first words alone
- (void)barrier:(bool)barrier
{
    std::lock_guard<raw::Mutex> hold(_buffer->_lock);
    RMICBDispatch &c = _buffer->_dispatches[_index];
    c.barrier = barrier;
    if (!c.raw)
        return;
    c.words.barrier = barrier;
    raw::icb::dispatch(computeSlot(_buffer, _index), uint32_t(_index), c.words);
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    if (_buffer->_inheritPipeline) {
        SWITCH_REFUSE("raw", "icb.inherited", "an indirect dispatch command set a pipeline its buffer inherits");
        return;
    }
    RMComputeState *s = [(id)state isKindOfClass:[RMComputeState class]] ? (RMComputeState *)state : nil;
    [self edit:^(RMICBDispatch &c) {
        c.pipeline = s;
    }];
}

- (void)setKernelBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self setKernelBuffer:buffer offset:offset attributeStride:NSUIntegerMax atIndex:index];
}

// A stage-in kernel's layout of MTLBufferLayoutStrideDynamic reads the stride (StageInStride); any other ignores it
- (void)setKernelBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride
                atIndex:(NSUInteger)index
{
    if (_buffer->_inheritBuffers || index >= 31) {
        SWITCH_REFUSE("raw", "icb.inherited",
                      "an indirect dispatch command set a buffer its buffer inherits or past 30");
        return;
    }
    RMBuffer *b = [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
    [self edit:^(RMICBDispatch &c) {
        c.buffers[index] = b;
        c.offsets[index] = offset;
        c.strides[index] = stride >= raw::kNoBindStride ? raw::kNoBindStride : uint32_t(stride);
    }];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    const uint32_t bytes = uint32_t((length + 15) & ~NSUInteger(15));
    [self edit:^(RMICBDispatch &c) {
        raw::ComputeSpecialization s;
        s.memory = c.memory;
        s.bindMemory(uint32_t(index), bytes);
        c.memory = s.memory;
    }];
}

- (void)setStageInRegion:(MTLRegion)region
{
    [self edit:^(RMICBDispatch &c) {
        c.stageIn = true;
        c.stageInRegion = region;
    }];
}

- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
    (void)width, (void)height;
    SWITCH_REFUSE("raw", "icb.imageblock",
                  "setImageblockWidth:height: on an indirect dispatch command: no imageblocks");
}

- (void)concurrentDispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:groups threads:threads perThread:false];
}

- (void)concurrentDispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:grid threads:threads perThread:true];
}

- (void)dispatch:(MTLSize)grid threads:(MTLSize)threads perThread:(bool)perThread
{
    [self edit:^(RMICBDispatch &c) {
        c.dispatched = true;
        c.perThread = perThread;
        c.grid = {grid.width, grid.height, grid.depth};
        c.threads = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    }];
}

- (void)setBarrier
{
    [self barrier:true];
}

- (void)clearBarrier
{
    [self barrier:false];
}

- (void)reset
{
    [self edit:^(RMICBDispatch &c) {
        c = {};
    }];
}

@end

@implementation RMKernelExecute
@end

id rmNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count)
{
    RMIndirectComputeBuffer *b = [RMIndirectComputeBuffer new];
    b->_device = device;
    b->_count = count;
    b->_descriptor = [d copy];
    b->_inheritPipeline = d.inheritPipelineState;
    b->_inheritBuffers = d.inheritBuffers;
    b->_dispatches.resize(count);
    if (!count || !gRM->raw->createBuffer(uint64_t(count) * raw::icb::kComputeSlotWords * 4, b->_raw, true)) {
        SWITCH_REFUSE("raw", "icb.memory", "no memory for an indirect command buffer of %lu dispatches",
                      (unsigned long)count);
        return nil;
    }
    for (NSUInteger i = 0; i < count; i++)
        raw::icb::computeEmpty(computeSlot(b, i));
    if (b->_inheritBuffers) {
        if (!gRM->raw->createBuffer(uint64_t(count) * raw::icb::kComputeBindSlotWords * 4, b->_binds, true)) {
            SWITCH_REFUSE("raw", "icb.memory",
                          "no memory for the bind slots of an indirect command buffer of %lu dispatches",
                          (unsigned long)count);
            return nil;
        }
        for (NSUInteger i = 0; i < count; i++)
            raw::icb::bindsEmpty(rmBindSlot(b, i));
    }
    return (id<MTLIndirectCommandBuffer>)b;
}
