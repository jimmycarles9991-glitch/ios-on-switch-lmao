// SPDX-License-Identifier: MIT
// A blit encoder's reset and copy of indirect commands (TegraGM20BMetalIndirect.h): on the copy engine, the words copied or filled
// in GPU order, each copied command's index word written for its new index; with no copy engine (the blit encoder replayed
// on m2v) the CPU writes them, as Metal's would be the GPU's. Buffers of different slot sizes (one that may hold patch
// commands and one that may not) copy command by command.
#import "TegraGM20BMetalIndirectCompute.h"

namespace {

// The copy engine's copy of commands [first, first + count) of `from` to `into` of `to`
void copySlots(gm20b::BlitEncoder *be, SwitchMetalIndirectCommandBuffer *from, SwitchMetalIndirectCommandBuffer *to,
               NSUInteger first, NSUInteger into, NSUInteger count)
{
    const uint64_t fromBytes = uint64_t(from->_slotWords) * 4, toBytes = uint64_t(to->_slotWords) * 4;
    if (fromBytes == toBytes) {
        be->copyBytes(from->_raw.va + first * fromBytes, to->_raw.va + into * toBytes, count * toBytes);
        return;
    }
    const uint64_t copied = std::min(fromBytes, toBytes);
    for (NSUInteger i = 0; i < count; i++) {
        const uint64_t at = to->_raw.va + (into + i) * toBytes;
        be->copyBytes(from->_raw.va + (first + i) * fromBytes, at, copied);
        if (toBytes > copied)
            be->fillWords(at + copied, toBytes - copied, gm20b::icb::kNop);
    }
}

}  // namespace

@implementation SwitchMetalBlitEncoder (Indirect)

- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    SwitchMetalIndirectCommandBuffer *b = [(id)buffer isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                              ? (SwitchMetalIndirectCommandBuffer *)buffer
                                              : nil;
    if (!b)
        return gm20bCallInvalid(_cb, PredCallIcb, range.location, range.length,
                                buffer ? "resetCommandsInBuffer: an indirect command buffer not made by this device"
                                       : "resetCommandsInBuffer: a nil indirect command buffer");
    // an M4 Max writes past the buffer: its next execute fails MTLCommandBufferErrorTimeout
    if (NSMaxRange(range) > b->_count)
        return gm20bCallInvalid(_cb, PredCallIcb, range.location, range.length,
                                "resetCommandsInBuffer: a range past its buffer");
    [_cb retain:b];
    Cmd m = cmd(OpResetCommands);
    m.obj = m.obj2 = (__bridge void *)b;
    m.c = range.location;
    m.x = range.length;
    [_cb record:m];
}

// A patch command copies only into a buffer that takes patch commands, as Metal's validation requires of the two
// buffers' command types
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                      sourceRange:(NSRange)range
                      destination:(id<MTLIndirectCommandBuffer>)destination
                 destinationIndex:(NSUInteger)index
{
    SwitchMetalIndirectCommandBuffer *from = [(id)source isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                                 ? (SwitchMetalIndirectCommandBuffer *)source
                                                 : nil;
    SwitchMetalIndirectCommandBuffer *to = [(id)destination isKindOfClass:[SwitchMetalIndirectCommandBuffer class]]
                                               ? (SwitchMetalIndirectCommandBuffer *)destination
                                               : nil;
    if (!from || !to)
        return gm20bCallInvalid(_cb, PredCallIcb, range.location, index,
                                source && destination
                                    ? "copyIndirectCommandBuffer: an indirect command buffer not made by this device"
                                    : "copyIndirectCommandBuffer: a nil indirect command buffer");
    if ([(id)from isKindOfClass:[SwitchMetalIndirectComputeBuffer class]] !=
            [(id)to isKindOfClass:[SwitchMetalIndirectComputeBuffer class]] ||
        (from->_patches && !to->_patches)) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 3,
                             "copyIndirectCommandBuffer: between buffers of draws and dispatches, or patch commands "
                             "into a buffer of none: none copied");
        return;
    }
    // an M4 Max copies the commands of the range inside both buffers
    if (NSMaxRange(range) > from->_count || index + range.length > to->_count) {
        gm20bSkipped(PredCallIcb);
        SWITCH_REFUSE_DETAIL("gm20b", "icb.range", 4, "copyIndirectCommandBuffer: a range past a buffer: cut to both");
        const NSUInteger fromLeft = range.location < from->_count ? from->_count - range.location : 0,
                         toLeft = index < to->_count ? to->_count - index : 0;
        range.length = std::min({range.length, fromLeft, toLeft});
        if (!range.length)
            return;
    }
    [_cb retain:from];
    [_cb retain:to];
    Cmd m = cmd(OpCopyCommands);
    m.obj = (__bridge void *)from;
    m.obj2 = (__bridge void *)to;
    m.c = range.location;
    m.x = range.length;
    m.a = uint32_t(index);
    [_cb record:m];
}

// Metal's optimization keeps what the commands draw: these are already the channel's words
- (void)optimizeIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    (void)buffer, (void)range;
}

@end

void encodeCommandsBlit(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const Cmd &c)
{
    if ([(__bridge id)c.obj2 isKindOfClass:[SwitchMetalIndirectComputeBuffer class]]) {
        encodeKernelCommandsBlit(rc, be, c);
        return;
    }
    SwitchMetalIndirectCommandBuffer *from = (__bridge SwitchMetalIndirectCommandBuffer *)c.obj;
    SwitchMetalIndirectCommandBuffer *to = (__bridge SwitchMetalIndirectCommandBuffer *)c.obj2;
    const uint64_t toBytes = uint64_t(to->_slotWords) * 4;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = c.op == OpCopyCommands ? c.a : first;
    if (c.op == OpCopyCommands)
        rc->use(from->_raw, false);
    rc->use(to->_raw, true, false);
    be->access(c.op == OpCopyCommands ? (__bridge void *)from : nullptr, (__bridge void *)to);
    std::vector<SwitchMetalICBCommand> commands(count);
    if (c.op == OpCopyCommands) {
        std::lock_guard<gm20b::UnfairMutex> hold(from->_lock);
        std::copy_n(from->_commands.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (c.op == OpResetCommands)
        be->fillWords(to->_raw.va + first * toBytes, count * toBytes, gm20b::icb::kNop);
    else
        copySlots(be, from, to, first, into, count);
    // a copied command draws as its new index (the gated draw compares it with the range)
    if (c.op == OpCopyCommands) {
        be->barrier();
        for (NSUInteger i = 0; i < count; i++)
            if (commands[i].kind != SwitchMetalICBCommand::None)
                be->fillWords(to->_raw.va + (into + i) * toBytes + gm20b::icb::kIndexWord * 4, 4, uint32_t(into + i));
    }
    to->_gpuWritten.store(true, std::memory_order_relaxed);
    for (NSUInteger i = 0; i < count; i++)
        [to take:commands[i] at:into + i];
}
