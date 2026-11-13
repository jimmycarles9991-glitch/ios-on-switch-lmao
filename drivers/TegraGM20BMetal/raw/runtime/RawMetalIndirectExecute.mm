// SPDX-License-Identifier: MIT
// A compute encoder's execute of an indirect command buffer of dispatches (raw_icb_compute.h) by reference: the buffer's own
// words, or its variant for the encoder's pipeline where it inherits that, and the bind slots for the encoder's buffers in
// the macros' scratch where it inherits them; m2v's replay encodes the commands again into m2v's buffer, which executes
// them as Metal does; and a blit's reset or copy of the commands.
#include "raw_lock.h"
#import "RawMetalIndirectCompute.h"

namespace {

// The commands an execute reaches: all of them where the GPU reads the range
std::pair<uint32_t, uint32_t> reach(RMKernelExecute *x)
{
    return x->_range ? std::make_pair(0u, uint32_t(x->_buffer->_count))
                     : std::make_pair(x->_first, x->_first + x->_count);
}

uint32_t *slotOf(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * raw::icb::kComputeSlotWords;
}

const char *const kNoKernel = "an indirect dispatch command the raw path has no kernel for at the encoder's pipeline";

// Each command's kernels at the encoder's pipeline, for a buffer with no variant for it at its generation
void pipelineKernels(RMKernelExecute *x)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    x->_kernels.assign(b->_count, nullptr);
    x->_kernelsGeneration = b->_generation;
    for (uint32_t i = 0; i < b->_count; i++) {
        raw::ComputeDispatch shape;
        if (b->_dispatches[i].dispatched)
            x->_kernels[i] = rmCommandKernels(x->_pipeline, b->_dispatches[i], shape);
    }
}

// Why the execute has no raw form (null: it has); a buffer that inherits the pipeline has its commands' kernels at the
// encoder's made now where it has no variant for it
const char *admit(RMKernelExecute *x)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<raw::Mutex> hold(b->_lock);
    if (b->_inheritPipeline) {
        const auto made = b->_variants.find(x->_pipeline);
        const bool current = made != b->_variants.end() && made->second->generation == b->_generation;
        if (!current)
            pipelineKernels(x);
        for (uint32_t i = from; i < to; i++) {
            const RMICBDispatch &c = b->_dispatches[i];
            if (c.dispatched && (current ? made->second->missing[i] : !x->_kernels[i]))
                return kNoKernel;
        }
        return nullptr;
    }
    for (uint32_t i = from; i < to; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        if (c.dispatched && !c.raw)
            return "an indirect dispatch command the raw path has no kernel for";
    }
    return nullptr;
}

std::vector<RMBuffer *> commandBuffers(RMKernelExecute *x)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::vector<RMBuffer *> out;
    std::lock_guard<raw::Mutex> hold(b->_lock);
    for (uint32_t i = from; i < to && !b->_inheritBuffers; i++)
        for (RMBuffer *buffer : b->_dispatches[i].buffers)
            if (buffer && std::find(out.begin(), out.end(), buffer) == out.end())
                out.push_back(buffer);
    return out;
}

}  // namespace

@implementation RMComputeEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self executeKernels:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` selects the commands: every command is referenced, and those outside
// it launch nothing (raw_mme.h kIcbGate)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    RMBuffer *r = [(id)rangeBuffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)rangeBuffer : nil;
    if (!r || (offset & 3) || offset > r->_raw.length || r->_raw.length - offset < 8) {
        SWITCH_REFUSE(
            "raw", "icb.range",
            "executeCommandsInBuffer:indirectBuffer: on a compute encoder: a range buffer not this device's or "
            "outside it: none run");
        return;
    }
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    [self executeKernels:buffer first:0 count:b ? b->_count : 0 range:r offset:offset];
}

- (void)executeKernels:(id<MTLIndirectCommandBuffer>)buffer
                 first:(NSUInteger)first
                 count:(NSUInteger)count
                 range:(RMBuffer *)rangeBuffer
                offset:(NSUInteger)offset
{
    RMIndirectComputeBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectComputeBuffer class]] ? (RMIndirectComputeBuffer *)buffer : nil;
    if (!b || !count || first + count > b->_count) {
        SWITCH_REFUSE(
            "raw", "icb.range",
            "executeCommandsInBuffer: on a compute encoder: a buffer of dispatches not this device's or a range "
            "past it: none run");
        return;
    }
    RMKernelExecute *x = [RMKernelExecute new];
    x->_buffer = b;
    x->_range = rangeBuffer;
    x->_rangeOffset = offset;
    x->_first = uint32_t(first);
    x->_count = uint32_t(count);
    x->_pipeline = [self rmState];
    if (const char *why = admit(x)) {
        [_cb takeReason:WhyCompute why:@(why)];
        hit(_cb, PredComputeKernel, first);
    }
    std::vector<raw::ResourceUse> uses;
    [_cb retain:b];
    [_cb retain:x];
    if (rangeBuffer) {
        [_cb retain:rangeBuffer];
        uses.push_back({(__bridge void *)rangeBuffer, raw::ResourceUse::Read});
    }
    for (RMBuffer *used : commandBuffers(x)) {
        [_cb retain:used];
        uses.push_back({(__bridge void *)used, raw::ResourceUse::Read | raw::ResourceUse::Write});
    }
    Cmd m = cmd(OpExecuteKernels, 3);
    m.obj = (__bridge void *)x;
    [_cb record:m];
    if (!uses.empty())
        _cb->_plan.addUses(_cb->_plan.size() - 1, uses);
}

@end

// The buffer's commands for the execute's pipeline as they are now: the variant made at an earlier execute, or made now
static std::shared_ptr<RMICBVariant> pipelineVariant(RMKernelExecute *x, const char **why)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<raw::Mutex> hold(b->_lock);
    std::shared_ptr<RMICBVariant> &v = b->_variants[x->_pipeline];
    if (!v || v->generation != b->_generation) {
        if (x->_kernelsGeneration != b->_generation || x->_kernels.size() != b->_count)
            pipelineKernels(x);
        std::shared_ptr<RMICBVariant> made = rmPipelineVariant(b, x->_pipeline, x->_kernels, why);
        if (!made)
            return nullptr;
        v = made;
    }
    for (uint32_t i = from; i < to; i++)
        if (v->missing[i]) {
            *why = kNoKernel;
            return nullptr;
        }
    return v;
}

bool encodeKernelCommands(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMKernelExecute *x, const char **why)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    for (RMBuffer *used : commandBuffers(x)) {
        useBuffer(&rc, used, true);
        if (used->_raw.hostVisible && !used->_raw.wc && used->_raw.cpu)
            rc.clean(used->_raw.cpu, used->_raw.length);
    }
    for (const raw::Buffer &chunk : b->_chunks)
        rc.use(chunk, false);
    raw::ComputeEncoder::IcbExecute run = {&b->_raw,
                                           x->_first,
                                           x->_first,
                                           x->_count,
                                           x->_range ? &x->_range->_raw : nullptr,
                                           x->_rangeOffset,
                                           b->_gpuWritten.load(std::memory_order_relaxed)};
    if (x->_range) {
        run.at = 0;
        run.first = 0;
        run.count = uint32_t(b->_count);
    }
    if (b->_inheritPipeline) {
        if (!(x->_variant = pipelineVariant(x, why)))
            return false;
        for (const raw::Buffer &chunk : x->_variant->chunks)
            rc.use(chunk, false);
        run.slots = &x->_variant->slots;
        run.written = false;
        if (b->_inheritBuffers) {
            run.binds = &x->_variant->binds;
            run.bindSlots = x->_variant->bindSlots;
            run.alignedSlots = x->_variant->alignedSlots;
            run.strideSlots = x->_variant->strideSlots;
        }
    } else if (b->_inheritBuffers) {
        std::lock_guard<raw::Mutex> hold(b->_lock);
        if (b->_bindsChanged) {
            b->_bindSlots = b->_alignedSlots = b->_strideSlots = 0;
            for (const RMICBDispatch &c : b->_dispatches) {
                b->_bindSlots |= c.bindSlots;
                b->_alignedSlots |= c.alignedSlots;
                b->_strideSlots |= c.strideSlots;
            }
            b->_bindsChanged = false;
        }
        run.binds = &b->_binds;
        run.bindSlots = b->_bindSlots;
        run.alignedSlots = b->_alignedSlots;
        run.strideSlots = b->_strideSlots;
    }
    if (x->_range)
        useBuffer(&rc, x->_range, false);
    return e.executeSlots(run, why);
}

void replayKernelCommands(id<MTLComputeCommandEncoder> encoder, RMKernelExecute *x, std::set<RMBuffer *> &buffers)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<raw::Mutex> hold(b->_lock);
    if (!b->_m2v)
        b->_m2v = [RM_M2V("an indirect command buffer of dispatches replayed on it")
            newIndirectCommandBufferWithDescriptor:b->_descriptor
                                   maxCommandCount:b->_count
                                           options:0];
    if (!b->_m2v) {
        SWITCH_REFUSE("raw", "icb.replay", "m2v made no indirect command buffer of dispatches: none run");
        return;
    }
    for (uint32_t i = from; i < to; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        id<MTLIndirectComputeCommand> t = [b->_m2v indirectComputeCommandAtIndex:i];
        [t reset];
        if (!b->_inheritPipeline && c.pipeline)
            [t setComputePipelineState:twinOf(c.pipeline)];
        for (uint32_t slot = 0; slot < 31 && !b->_inheritBuffers; slot++)
            if (RMBuffer *used = c.buffers[slot]) {
                buffers.insert(used);
                if (c.strides[slot] == raw::kNoBindStride)
                    [t setKernelBuffer:twinOf(used) offset:c.offsets[slot] atIndex:slot];
                else
                    [t setKernelBuffer:twinOf(used)
                                 offset:c.offsets[slot]
                        attributeStride:c.strides[slot]
                                atIndex:slot];
            }
        for (const auto &[index, bytes] : c.memory)
            [t setThreadgroupMemoryLength:bytes atIndex:index];
        if (c.stageIn)
            [t setStageInRegion:c.stageInRegion];
        if (c.barrier)
            [t setBarrier];
        if (!c.dispatched)
            continue;
        const MTLSize grid = MTLSizeMake(c.grid[0], c.grid[1], c.grid[2]),
                      threads = MTLSizeMake(c.threads[0], c.threads[1], c.threads[2]);
        if (c.perThread)
            [t concurrentDispatchThreads:grid threadsPerThreadgroup:threads];
        else
            [t concurrentDispatchThreadgroups:grid threadsPerThreadgroup:threads];
    }
    if (x->_range) {
        buffers.insert(x->_range);
        [encoder executeCommandsInBuffer:b->_m2v indirectBuffer:twinOf(x->_range) indirectBufferOffset:x->_rangeOffset];
    } else {
        [encoder executeCommandsInBuffer:b->_m2v withRange:NSMakeRange(x->_first, x->_count)];
    }
}

void encodeKernelCommandsBlit(raw::BlitEncoder *be, const Cmd &c)
{
    RMIndirectComputeBuffer *from = (__bridge RMIndirectComputeBuffer *)c.obj;
    RMIndirectComputeBuffer *to = (__bridge RMIndirectComputeBuffer *)c.obj2;
    constexpr uint64_t kSlotBytes = raw::icb::kComputeSlotWords * 4;
    const bool copy = c.op == OpCopyCommands;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = copy ? c.a : first;
    std::vector<RMICBDispatch> commands(count);
    if (copy) {
        std::lock_guard<raw::Mutex> hold(from->_lock);
        std::copy_n(from->_dispatches.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (be) {
        be->access(copy ? (__bridge void *)from : nullptr, (__bridge void *)to);
        if (!copy)
            be->fillWords(to->_raw.va + first * kSlotBytes, count * kSlotBytes, raw::icb::kNop);
        else
            be->copyBytes(from->_raw.va + first * kSlotBytes, to->_raw.va + into * kSlotBytes, count * kSlotBytes);
        // a copied command launches as its new index (the gate compares it with the range)
        if (copy) {
            be->barrier();
            for (NSUInteger i = 0; i < count; i++)
                if (commands[i].raw)
                    be->fillWords(to->_raw.va + (into + i) * kSlotBytes + raw::icb::kComputeIndexWord * 4, 4,
                                  uint32_t(into + i));
        }
        if (to->_inheritBuffers) {
            constexpr uint64_t kBindBytes = raw::icb::kComputeBindSlotWords * 4;
            if (copy && from->_inheritBuffers)
                be->copyBytes(from->_binds.va + first * kBindBytes, to->_binds.va + into * kBindBytes,
                              count * kBindBytes);
            else
                be->fillWords(to->_binds.va + into * kBindBytes, count * kBindBytes, raw::icb::kNop);
        }
        to->_gpuWritten.store(true, std::memory_order_relaxed);
    }
    std::lock_guard<raw::Mutex> hold(to->_lock);
    to->_bindsChanged = true;
    to->_generation++;
    for (NSUInteger i = 0; i < count; i++) {
        if (copy && !from->_inheritBuffers) {
            commands[i].binds.clear();
            commands[i].strideWords.clear();
            commands[i].bindSlots = commands[i].alignedSlots = commands[i].strideSlots = 0;
        }
        to->_dispatches[into + i] = commands[i];
        // with no copy engine (the blit encoder replayed on m2v) the words are the CPU's to write, as Metal's would be the GPU's
        if (be)
            continue;
        if (commands[i].raw)
            raw::icb::dispatch(slotOf(to, into + i), uint32_t(into + i), commands[i].words);
        else
            raw::icb::computeEmpty(slotOf(to, into + i));
        if (to->_inheritBuffers)
            raw::icb::binds(rmBindSlot(to, into + i), commands[i].binds.data(), uint32_t(commands[i].binds.size()),
                            commands[i].strideWords.data(), uint32_t(commands[i].strideWords.size()));
    }
}
