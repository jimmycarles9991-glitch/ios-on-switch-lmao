// SPDX-License-Identifier: MIT
// Indirect command buffers (TegraGM20BMetalIndirect.mm, TegraGM20BMetalIndirectPatches.mm, TegraGM20BMetalIndirectBlit.mm, icb.h)
#pragma once
#import "TegraGM20BMetalInternal.h"
#include <unordered_map>
#include "icb.h"

// A command as the CPU encoded it, beside its words: the kinds and buffers its buffer holds (take:at:), which a range
// reads
struct SwitchMetalICBCommand {
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
    SwitchMetalBuffer *indices = nil;
    gm20b::icb::PatchDraw patches{};  // a patch command's words
    SwitchMetalBuffer *factors = nil, *patchIndices = nil, *pointIndices = nil;
    // what the CPU encoded into its index that Metal's validation stops and Apple's driver crashes on, or an object of
    // another device: an execute that reaches it fails InvalidResource (gm20bCallInvalid), until a reset of it
    SwitchMetalPred invalid = PredNone;
    const char *invalidWhy = nullptr;
};

// The kinds of a buffer's commands (SwitchMetalIndirectCommandBuffer kinds): triangles and lines or points take different polygon
// offsets (gm20b::icb::polygonOffset), patches a tessellation state
enum SwitchMetalICBKinds : uint8_t {
    SwitchMetalICBTriangles = 1,
    SwitchMetalICBOthers = 2,
    SwitchMetalICBPatches = 4,
    SwitchMetalICBIndexed = 8
};

@interface SwitchMetalIndirectCommandBuffer : SwitchMetalObject {
@public
    gm20b::Buffer _raw;  // the commands' words, _slotWords each
    NSUInteger _count;
    uint32_t _slotWords;  // gm20b::icb::kPatchSlotWords where its descriptor has patch commands, else kSlotWords
    bool _patches;        // its descriptor's command types hold drawPatches: or drawIndexedPatches: (and the device's)
    gm20b::UnfairMutex _lock;   // commands are encoded from any thread
    std::vector<SwitchMetalICBCommand> _commands;
    std::unordered_map<void *, uint32_t> _buffers;  // the buffers its commands read, and how many name each
    uint32_t _kinds[4];                             // its commands of each SwitchMetalICBKinds bit
    // a blit encoder's reset or copy wrote its words on the GPU: an execute fetches them after the GPU's writes land
    std::atomic<bool> _gpuWritten;
    std::atomic<uint32_t> _invalid;  // its commands marked invalid (SwitchMetalICBCommand::invalid)
}
// Command `index` is now `command` (its words already written); a command the CPU encodes over one marked invalid keeps
// the mark, which only a reset (a command of none) clears
- (void)take:(SwitchMetalICBCommand)command at:(NSUInteger)index;
- (uint8_t)kinds;
- (std::vector<SwitchMetalBuffer *>)buffers;
// The first of commands [first, first + count) marked invalid, its mark in `why`; PredNone: none
- (SwitchMetalPred)invalidIn:(NSUInteger)first count:(NSUInteger)count why:(const char **)why;
@end

@interface SwitchMetalIndirectRenderCommand : SwitchMetalObject {
@public
    SwitchMetalIndirectCommandBuffer *_buffer;
    NSUInteger _index;
}
- (void)reset;
// Its words emptied and its index marked invalid with `p` and `why`
- (void)invalidate:(SwitchMetalPred)p why:(const char *)why;
@end

inline uint32_t *gm20bCommandSlot(SwitchMetalIndirectCommandBuffer *b, NSUInteger index)
{
    b->_raw.markCpuDirty();
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * b->_slotWords;
}

// The raw encode of OpExecuteCommands: each command drawn as the encoder's own draw
bool encodeCommands(gm20b::RenderEncoder *re, gm20b::CommandBuffer *rc, const SwitchMetalPlanDraw &draw,
                    const char **why);
// OpResetCommands and OpCopyCommands on the copy engine (`be`)
void encodeCommandsBlit(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const Cmd &c);
