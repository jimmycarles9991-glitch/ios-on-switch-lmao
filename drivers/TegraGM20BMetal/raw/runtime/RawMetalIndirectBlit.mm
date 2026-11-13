// SPDX-License-Identifier: MIT
// A blit encoder's reset and copy of indirect commands (RawMetalIndirect.h): on the copy engine, the words copied or filled
// in GPU order, each copied command's index word written for its new index; with no copy engine (the blit encoder replayed
// on m2v) the CPU writes them, as Metal's would be the GPU's. Buffers of different slot sizes (one that may hold patch
// commands and one that may not) copy command by command.
#import "RawMetalIndirectCompute.h"

namespace {

// The copy engine's copy of commands [first, first + count) of `from` to `into` of `to`
void copySlots(raw::BlitEncoder *be, RMIndirectCommandBuffer *from, RMIndirectCommandBuffer *to, NSUInteger first,
               NSUInteger into, NSUInteger count)
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
            be->fillWords(at + copied, toBytes - copied, raw::icb::kNop);
    }
}

}  // namespace

@implementation RMBlitEncoder (Indirect)

- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    if (!b || NSMaxRange(range) > b->_count) {
        SWITCH_REFUSE("raw", "icb.range", "resetCommandsInBuffer: a buffer not this device's or a range past it");
        return;
    }
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
    RMIndirectCommandBuffer *from =
        [(id)source isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)source : nil;
    RMIndirectCommandBuffer *to =
        [(id)destination isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)destination : nil;
    if (!from || !to || NSMaxRange(range) > from->_count || index + range.length > to->_count ||
        [(id)from isKindOfClass:[RMIndirectComputeBuffer class]] !=
            [(id)to isKindOfClass:[RMIndirectComputeBuffer class]] ||
        (from->_patches && !to->_patches)) {
        SWITCH_REFUSE("raw", "icb.range",
                      "copyIndirectCommandBuffer: a buffer not this device's, a range past one, or patch commands into "
                      "a buffer of none");
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

void encodeCommandsBlit(raw::BlitEncoder *be, const Cmd &c)
{
    if ([(__bridge id)c.obj2 isKindOfClass:[RMIndirectComputeBuffer class]]) {
        encodeKernelCommandsBlit(be, c);
        return;
    }
    RMIndirectCommandBuffer *from = (__bridge RMIndirectCommandBuffer *)c.obj;
    RMIndirectCommandBuffer *to = (__bridge RMIndirectCommandBuffer *)c.obj2;
    const uint64_t toBytes = uint64_t(to->_slotWords) * 4;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = c.op == OpCopyCommands ? c.a : first;
    if (be) {
        be->access(c.op == OpCopyCommands ? (__bridge void *)from : nullptr, (__bridge void *)to);
    }
    std::vector<RMICBCommand> commands(count);
    if (c.op == OpCopyCommands) {
        std::lock_guard<raw::Mutex> hold(from->_lock);
        std::copy_n(from->_commands.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (be) {
        if (c.op == OpResetCommands)
            be->fillWords(to->_raw.va + first * toBytes, count * toBytes, raw::icb::kNop);
        else
            copySlots(be, from, to, first, into, count);
        // a copied command draws as its new index (the gated draw compares it with the range)
        if (c.op == OpCopyCommands) {
            be->barrier();
            for (NSUInteger i = 0; i < count; i++)
                if (commands[i].kind != RMICBCommand::None)
                    be->fillWords(to->_raw.va + (into + i) * toBytes + raw::icb::kIndexWord * 4, 4, uint32_t(into + i));
        }
        to->_gpuWritten.store(true, std::memory_order_relaxed);
    }
    for (NSUInteger i = 0; i < count; i++) {
        if (!be)
            rmWriteCommand(to, into + i, commands[i]);
        [to take:commands[i] at:into + i];
    }
}
