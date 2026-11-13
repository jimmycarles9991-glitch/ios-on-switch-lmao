// SPDX-License-Identifier: MIT
// Indirect command buffers (RawMetalIndirect.mm, RawMetalIndirectPatches.mm, RawMetalIndirectBlit.mm, raw_icb.h)
#pragma once
#import "RawMetalInternal.h"
#include <functional>
#include <unordered_map>
#include "raw_icb.h"

// A command as the CPU encoded it, beside its words: what m2v's replay draws, a blit's copy writes again without the copy
// engine, and which buffers a range reads
struct RMICBCommand {
    enum Kind : uint8_t {
        None,
        Draw,
        Indexed,
        Patches
    } kind = None;

    uint8_t primitive = 0, indexType = 0;
    uint32_t first = 0, count = 0, instances = 0, baseInstance = 0;
    int32_t baseVertex = 0;
    uint64_t indexOffset = 0;
    RMBuffer *indices = nil;
    raw::icb::PatchDraw patches{};  // a patch command's words
    RMBuffer *factors = nil, *patchIndices = nil, *pointIndices = nil;
};

// The kinds of a buffer's commands (RMIndirectCommandBuffer kinds): triangles and lines or points take different raster
// state (raw::icb::triangles), patches a tessellation state
enum RMICBKinds : uint8_t {
    RMICBTriangles = 1,
    RMICBOthers = 2,
    RMICBPatches = 4,
    RMICBIndexed = 8
};

@interface RMIndirectCommandBuffer : RMObject {
@public
    raw::Buffer _raw;  // the commands' words, _slotWords each
    NSUInteger _count;
    uint32_t _slotWords;  // raw::icb::kPatchSlotWords where its descriptor has patch commands, else kSlotWords
    bool _patches;        // its descriptor's command types hold drawPatches: or drawIndexedPatches: (and the device's)
    raw::Mutex _lock;     // commands are encoded from any thread
    std::vector<RMICBCommand> _commands;
    std::unordered_map<void *, uint32_t> _buffers;  // the buffers its commands read, and how many name each
    uint32_t _kinds[4];                             // its commands of each RMICBKinds bit
    // a blit encoder's reset or copy wrote its words on the GPU: an execute fetches them after the GPU's writes land
    std::atomic<bool> _gpuWritten;
}
// Command `index` is now `command` (its words already written)
- (void)take:(RMICBCommand)command at:(NSUInteger)index;
- (uint8_t)kinds;
- (std::vector<RMBuffer *>)buffers;
@end

@interface RMIndirectRenderCommand : RMObject {
@public
    RMIndirectCommandBuffer *_buffer;
    NSUInteger _index;
}
- (void)reset;
@end

inline uint32_t *rmCommandSlot(RMIndirectCommandBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * b->_slotWords;
}

// Command `index`'s words as `c` says, written by the CPU
void rmWriteCommand(RMIndirectCommandBuffer *b, NSUInteger index, const RMICBCommand &c);

// The raw encode of OpExecuteCommands, and m2v's replay of it: each command drawn as the encoder's own draw
bool encodeCommands(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const char **why);
void replayCommands(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const std::function<id(void *)> &use);
// OpResetCommands and OpCopyCommands: on the copy engine (`be`), or with none (m2v's replay) the commands as the CPU keeps
// them alone, which every encode brings to the commands' state in its command buffer's order
void encodeCommandsBlit(raw::BlitEncoder *be, const Cmd &c);
