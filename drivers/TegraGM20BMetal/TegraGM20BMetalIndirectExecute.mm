// SPDX-License-Identifier: MIT
// A compute encoder's execute of an indirect command buffer of dispatches (icb_compute.h) by reference: the buffer's own
// words, or its variant for the encoder's pipeline where it inherits that, and the bind slots for the encoder's buffers in
// the macros' scratch where it inherits them; and a blit's reset or copy of the commands.
#include "lock.h"
#import "TegraGM20BMetalIndirectCompute.h"

namespace {

// The commands an execute reaches: all of them where the GPU reads the range
std::pair<uint32_t, uint32_t> reach(SwitchMetalKernelExecute *x)
{
    return x->_range ? std::make_pair(0u, uint32_t(x->_buffer->_count))
                     : std::make_pair(x->_first, x->_first + x->_count);
}

// The copy engine's copy of `count` slots of `fromWords` at `from` to slots of `toWords` at `to`, which hold every
// copied command: one copy where the sizes match, else each slot's words, the rest of a longer slot no-ops
void copySlots(gm20b::BlitEncoder *be, uint64_t from, uint32_t fromWords, uint64_t to, uint32_t toWords,
               NSUInteger count)
{
    const uint64_t fromBytes = uint64_t(fromWords) * 4, toBytes = uint64_t(toWords) * 4;
    if (fromBytes == toBytes) {
        be->copyBytes(from, to, count * toBytes);
        return;
    }
    const uint64_t copied = std::min(fromBytes, toBytes);
    for (NSUInteger i = 0; i < count; i++) {
        be->copyBytes(from + i * fromBytes, to + i * toBytes, copied);
        if (toBytes > copied)
            be->fillWords(to + i * toBytes + copied, toBytes - copied, gm20b::icb::kNop);
    }
}

// `b`'s slots and bind slots made to hold `commands` (under its lock). False: no memory for that, and each of them that
// launches launches nothing, an execute that reaches it failed as out of memory until it is written again.
bool fitCommands(SwitchMetalIndirectComputeBuffer *b, std::vector<SwitchMetalICBDispatch> &commands)
{
    uint64_t slotWords = 0, bindWords = 0;
    for (const SwitchMetalICBDispatch &c : commands) {
        slotWords = std::max(slotWords, gm20bSlotWords(c));
        bindWords = std::max(bindWords, gm20bBindWords(c));
    }
    if (gm20bFitSlots(b, slotWords, bindWords))
        return true;
    for (SwitchMetalICBDispatch &c : commands)
        if (c.raw) {
            c.raw = false;
            c.unmade = {"no memory for its buffer's slots of its launches or binds",
                        gm20b::ComputeFailure::OutOfMemory};
        }
    return false;
}

// Each command's kernels at the encoder's pipeline, for a buffer with no variant for it at its generation
void pipelineKernels(SwitchMetalKernelExecute *x)
{
    SwitchMetalIndirectComputeBuffer *b = x->_buffer;
    x->_kernels.assign(b->_count, nullptr);
    x->_unmade.assign(b->_count, SwitchMetalICBUnmade{});
    x->_kernelsGeneration = b->_generation;
    for (uint32_t i = 0; i < b->_count; i++) {
        gm20b::ComputeDispatch shape;
        x->_kernels[i] = gm20bCommandKernels(x->_pipeline, b->_dispatches[i], shape, x->_unmade[i]);
    }
}

bool outOfMemory(const SwitchMetalICBUnmade &u)
{
    return u.why && u.failure == gm20b::ComputeFailure::OutOfMemory;
}

// The first command the execute reaches that dispatches and launches nothing of its own (`why` null: none); a buffer
// that inherits the pipeline has its commands' kernels at the encoder's made now where it has no variant for it. A
// command its writer had no memory for is written again first, its variant made again, so it runs once there is.
SwitchMetalICBUnmade admit(SwitchMetalKernelExecute *x)
{
    SwitchMetalIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<gm20b::UnfairMutex> hold(b->_lock);
    const std::vector<SwitchMetalICBUnmade> *unmade = &x->_unmade;
    if (b->_inheritPipeline) {
        const auto made = b->_variants.find(x->_pipeline);
        const bool current =
            made != b->_variants.end() && made->second->generation == b->_generation &&
            std::none_of(made->second->missing.begin() + from, made->second->missing.begin() + to, outOfMemory);
        if (current) {
            unmade = &made->second->missing;
        } else {
            if (made != b->_variants.end())
                b->_variants.erase(made);  // an execute in flight keeps its own reference
            pipelineKernels(x);
        }
    } else {
        for (uint32_t i = from; i < to; i++)
            if (outOfMemory(b->_dispatches[i].unmade))
                gm20bWriteCommand(b, i);
    }
    for (uint32_t i = from; i < to; i++)
        if (const SwitchMetalICBUnmade &u = b->_inheritPipeline ? (*unmade)[i] : b->_dispatches[i].unmade; u.why)
            return u;
    return {};
}

std::vector<SwitchMetalBuffer *> commandBuffers(SwitchMetalKernelExecute *x)
{
    SwitchMetalIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::vector<SwitchMetalBuffer *> out;
    std::lock_guard<gm20b::UnfairMutex> hold(b->_lock);
    for (uint32_t i = from; i < to && !b->_inheritBuffers; i++)
        for (SwitchMetalBuffer *buffer : b->_dispatches[i].buffers)
            if (buffer && std::find(out.begin(), out.end(), buffer) == out.end())
                out.push_back(buffer);
    return out;
}

}  // namespace

@implementation SwitchMetalComputeEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self executeKernels:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` selects the commands: every command is referenced, and those outside
// it launch nothing (mme.h kIcbGate)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    SwitchMetalBuffer *r =
        [(id)rangeBuffer isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)rangeBuffer : nil;
    if (!r)
        return gm20bCallInvalid(_cb, PredCallBufForeign, offset, 0,
                                rangeBuffer ? "an execute's range buffer not made by this device"
                                            : "an execute's nil range buffer");
    // an M4 Max's GPU reads the range at the offset rounded down to 4 bytes, and past its buffer reads whatever lies
    // there: nothing launched, commands launched or a page fault, by those bytes
    const NSUInteger read = offset & ~NSUInteger(3);
    if (read > r->_raw.length || r->_raw.length - read < 8) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 5,
                             "executeCommandsInBuffer:indirectBuffer: on a compute encoder: a range read past its "
                             "buffer: none run");
        return;
    }
    if (read != offset) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL(
            "gm20b", "icb.range", 10,
            "executeCommandsInBuffer:indirectBuffer: on a compute encoder: a range offset off 4 bytes: "
            "read from it rounded down");
    }
    SwitchMetalIndirectCommandBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                              ? (SwitchMetalIndirectCommandBuffer *)buffer
                                              : nil;
    [self executeKernels:buffer first:0 count:b ? b->_count : 0 range:r offset:read];
}

- (void)executeKernels:(id<MTLIndirectCommandBuffer>)buffer
                 first:(NSUInteger)first
                 count:(NSUInteger)count
                 range:(SwitchMetalBuffer *)rangeBuffer
                offset:(NSUInteger)offset
{
    SwitchMetalIndirectComputeBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalIndirectComputeBuffer class]]
                                              ? (SwitchMetalIndirectComputeBuffer *)buffer
                                              : nil;
    if (!b && ![(id)buffer isKindOfClass:[SwitchMetalIndirectCommandBuffer class]])
        return gm20bCallInvalid(_cb, PredCallIcb, first, count,
                                buffer ? "an execute of an indirect command buffer not made by this device"
                                       : "an execute of a nil indirect command buffer");
    if (!b) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 6,
                             "executeCommandsInBuffer: a buffer of draws on a compute encoder: "
                             "none run");
        return;
    }
    // an M4 Max launches the commands of the range inside the buffer
    if (first + count > b->_count) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 7,
                             "executeCommandsInBuffer: on a compute encoder: a range past its buffer: cut to it");
        count = first < b->_count ? b->_count - first : 0;
    }
    if (!count)
        return;
    SwitchMetalKernelExecute *x = [SwitchMetalKernelExecute new];
    x->_buffer = b;
    x->_range = rangeBuffer;
    x->_rangeOffset = offset;
    x->_first = uint32_t(first);
    x->_count = uint32_t(count);
    x->_pipeline = [self gm20bState];
    if (const SwitchMetalICBUnmade unmade = admit(x); unmade.why)
        return gm20bComputeFails(_cb, unmade.failure, unmade.pred, first, unmade.why);
    std::vector<gm20b::ResourceUse> uses;
    [_cb retain:b];
    [_cb retain:x];
    if (rangeBuffer) {
        [_cb retain:rangeBuffer];
        uses.push_back({(__bridge void *)rangeBuffer, gm20b::ResourceUse::Read});
    }
    for (SwitchMetalBuffer *used : commandBuffers(x)) {
        [_cb retain:used];
        uses.push_back({(__bridge void *)used, gm20b::ResourceUse::Read | gm20b::ResourceUse::Write});
    }
    Cmd m = cmd(OpExecuteKernels, 3);
    m.obj = (__bridge void *)x;
    [_cb record:m];
    if (!uses.empty())
        _cb->_plan.addUses(_cb->_plan.size() - 1, uses);
}

@end

// The buffer's commands for the execute's pipeline as they are now: the variant made at an earlier execute, or made
// now. Null where the encoder `e` refuses them (*why, e.failure()).
static std::shared_ptr<SwitchMetalICBVariant> pipelineVariant(SwitchMetalKernelExecute *x, gm20b::ComputeEncoder &e,
                                                              const char **why)
{
    SwitchMetalIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<gm20b::UnfairMutex> hold(b->_lock);
    std::shared_ptr<SwitchMetalICBVariant> &v = b->_variants[x->_pipeline];
    if (!v || v->generation != b->_generation) {
        if (x->_kernelsGeneration != b->_generation || x->_kernels.size() != b->_count)
            pipelineKernels(x);
        std::shared_ptr<SwitchMetalICBVariant> made =
            gm20bPipelineVariant(b, x->_pipeline, x->_kernels, x->_unmade, why);
        if (!made) {
            e.fails(gm20b::ComputeFailure::OutOfMemory);
            return nullptr;
        }
        v = made;
    }
    for (uint32_t i = from; i < to; i++)
        if (v->missing[i].why) {
            *why = v->missing[i].why;
            e.fails(v->missing[i].failure);
            return nullptr;
        }
    return v;
}

bool encodeKernelCommands(gm20b::ComputeEncoder &e, gm20b::CommandBuffer &rc, SwitchMetalKernelExecute *x,
                          const char **why)
{
    SwitchMetalIndirectComputeBuffer *b = x->_buffer;
    for (SwitchMetalBuffer *used : commandBuffers(x)) {
        useBuffer(&rc, used, true);
        if (used->_raw.hostVisible && !used->_raw.wc && used->_raw.cpu)
            rc.clean(used->_raw.cpu, used->_raw.length);
    }
    gm20b::ComputeEncoder::IcbExecute run = {nullptr,
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
    gm20b::Buffer slots, binds;  // the buffer's own as they are now: a command encoded later may grow them
    {
        std::lock_guard<gm20b::UnfairMutex> hold(b->_lock);
        for (const gm20b::Buffer &chunk : b->_chunks)
            rc.use(chunk, false);
        slots = b->_raw;
        binds = b->_binds;
        run.slots = &slots;
        run.slotWords = b->_slotWords;
        if (b->_inheritBuffers && !b->_inheritPipeline) {
            if (b->_bindsChanged) {
                b->_bindSlots = b->_strideSlots = 0;
                for (const SwitchMetalICBDispatch &c : b->_dispatches) {
                    b->_bindSlots |= c.bindSlots;
                    b->_strideSlots |= c.strideSlots;
                }
                b->_bindsChanged = false;
            }
            run.binds = &binds;
            run.bindWords = b->_bindWords;
            run.bindSlots = b->_bindSlots;
            run.strideSlots = b->_strideSlots;
        }
    }
    if (b->_inheritPipeline) {
        if (!(x->_variant = pipelineVariant(x, e, why)))
            return false;
        for (const gm20b::Buffer &chunk : x->_variant->chunks)
            rc.use(chunk, false);
        run.slots = &x->_variant->slots;
        run.slotWords = x->_variant->slotWords;
        run.written = false;
        if (b->_inheritBuffers) {
            run.binds = &x->_variant->binds;
            run.bindWords = x->_variant->bindWords;
            run.bindSlots = x->_variant->bindSlots;
            run.strideSlots = x->_variant->strideSlots;
        }
    }
    if (x->_range)
        useBuffer(&rc, x->_range, false);
    return e.executeSlots(run, why);
}

void encodeKernelCommandsBlit(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const Cmd &c)
{
    SwitchMetalIndirectComputeBuffer *from = (__bridge SwitchMetalIndirectComputeBuffer *)c.obj;
    SwitchMetalIndirectComputeBuffer *to = (__bridge SwitchMetalIndirectComputeBuffer *)c.obj2;
    const bool copy = c.op == OpCopyCommands;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = copy ? c.a : first;
    std::vector<SwitchMetalICBDispatch> commands(count);
    if (copy) {
        std::lock_guard<gm20b::UnfairMutex> hold(from->_lock);
        std::copy_n(from->_dispatches.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (copy && !from->_inheritBuffers)
        for (SwitchMetalICBDispatch &command : commands) {
            command.binds.clear();
            command.strideWords.clear();
            command.roots.clear();
            command.bindSlots = command.strideSlots = 0;
        }
    gm20b::Buffer toSlots, toBinds;
    uint32_t toWords, toBindWords;
    bool copied;
    {
        std::lock_guard<gm20b::UnfairMutex> hold(to->_lock);
        copied = fitCommands(to, commands) && copy;
        toSlots = to->_raw;
        toBinds = to->_binds;
        toWords = to->_slotWords;
        toBindWords = to->_bindWords;
    }
    gm20b::Buffer fromSlots, fromBinds;
    uint32_t fromWords = 0, fromBindWords = 0;
    if (copied) {
        std::lock_guard<gm20b::UnfairMutex> hold(from->_lock);
        fromSlots = from->_raw;
        fromBinds = from->_binds;
        fromWords = from->_slotWords;
        fromBindWords = from->_bindWords;
        rc->use(fromSlots, false);
    }
    const uint64_t toBytes = uint64_t(toWords) * 4, toBindBytes = uint64_t(toBindWords) * 4;
    rc->use(toSlots, true, false);
    if (to->_inheritBuffers) {
        if (copied && from->_inheritBuffers)
            rc->use(fromBinds, false);
        rc->use(toBinds, true, false);
    }
    be->access(copy ? (__bridge void *)from : nullptr, (__bridge void *)to);
    if (!copied)
        be->fillWords(toSlots.va + into * toBytes, count * toBytes, gm20b::icb::kNop);
    else
        copySlots(be, fromSlots.va + first * uint64_t(fromWords) * 4, fromWords, toSlots.va + into * toBytes, toWords,
                  count);
    // a copied command launches as its new index (the gate compares it with the range)
    if (copied) {
        be->barrier();
        for (NSUInteger i = 0; i < count; i++)
            if (commands[i].raw)
                be->fillWords(toSlots.va + (into + i) * toBytes + gm20b::icb::kComputeIndexWord * 4, 4,
                              uint32_t(into + i));
    }
    if (to->_inheritBuffers) {
        if (copied && from->_inheritBuffers)
            copySlots(be, fromBinds.va + first * uint64_t(fromBindWords) * 4, fromBindWords,
                      toBinds.va + into * toBindBytes, toBindWords, count);
        else
            be->fillWords(toBinds.va + into * toBindBytes, count * toBindBytes, gm20b::icb::kNop);
    }
    to->_gpuWritten.store(true, std::memory_order_relaxed);
    std::lock_guard<gm20b::UnfairMutex> hold(to->_lock);
    to->_bindsChanged = true;
    to->_generation++;
    for (NSUInteger i = 0; i < count; i++)
        to->_dispatches[into + i] = commands[i];
}
