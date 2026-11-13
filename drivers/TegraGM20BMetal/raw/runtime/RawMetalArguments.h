// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

enum Op : uint8_t {
    OpRender,
    OpBlit,
    OpCompute,
    OpEnd,
    OpPipeline,
    OpBytes,
    OpBuffer,
    OpBufferOffset,
    OpTexture,
    OpSampler,
    OpScissor,
    OpViewport,
    OpDraw,
    OpCull,
    OpWinding,
    OpDepthStencil,
    OpFillMode,
    OpBlendColor,
    OpStencilRef,
    OpDepthBias,
    OpBarrier,
    OpUse,
    OpUpdateFence,
    OpWaitFence,
    OpCopyTexture,
    OpCopyBuffer,
    OpCopyBufferToTexture,
    OpCopyTextureToBuffer,
    OpMipmaps,
    OpInvocation,
    OpSignalEvent,
    OpWaitEvent,
    OpDrawIndexed,
    OpVisibility,
    OpFillBuffer,
    OpFillTexture,
    // a compute encoder's (RawMetalCompute.mm)
    OpKernelState,
    OpKernelBytes,
    OpKernelBuffer,
    OpKernelBufferOffset,
    OpKernelClockAnchors,  // the kernel's clock anchor table (RawMetalCounters.h) at c, b bytes long, as buffer `index`
    OpKernelTexture,
    OpKernelSampler,
    OpKernelMemory,
    OpKernelUse,
    OpKernelBarrier,
    OpDispatch,
    OpDrawIndirect,
    OpDrawIndexedIndirect,
    OpExecuteCommands,  // an indirect command buffer's range: obj2 it, x the first command, c the count (RawMetalIndirect.mm)
    OpResetCommands,  // obj and obj2 the buffer, c the first command, x the count
    OpCopyCommands,  // obj the source, obj2 the destination, c the source's first command, x the count, a the destination's
    // a compute encoder's predication record (raw_predicate_records.h kind in `stage`): the buffer, the word's offset in
    // `c`, the compare (raw::PredicateCompare) in `a`, the reference in `b`
    OpPredicate,
    OpExecuteKernels,  // a compute encoder's execute of a buffer of dispatches: obj its RMKernelExecute (RawMetalIndirectExecute.mm)
    OpDepthClip,  // setDepthClipMode: `a` the MTLDepthClipMode
    // A tessellation state's draws (RawMetalTessellation.mm): a, b the patch start and count, c the instances << 32 | the base
    // instance, obj the patch index buffer, obj2 the indirect arguments, x the PatchExtra words
    OpDrawPatches,
    OpDrawPatchesIndirect,
    // setVertexAmplificationCount:viewMappings: (RawMetalAmplification.mm): a the count, x kAmplificationExtraWords words of
    // view mappings, viewport then render target offset each (0 for nil mappings)
    OpAmplification,
};

constexpr uint32_t kAmplificationMappings = 8, kAmplificationExtraWords = 2 * kAmplificationMappings;

// A patch draw's extra words: its patch index buffer's offset, its arguments' offset, the encoder's factor buffer, the draw's
// control-point index buffer, their offsets, the factors' instance stride, the draw's control points and the encoder's scale's
// bits
enum PatchExtra : uint8_t {
    PatchIndexOffset,
    PatchArgumentOffset,
    PatchFactors,
    PatchPointIndices,
    PatchFactorOffset,
    PatchPointIndexOffset,
    PatchFactorStride,
    PatchControlPoints,
    PatchScale,
    PatchExtraWords
};

// Transient adapter arguments. Native command plans do not store setter records.
struct Cmd {
    Op op;
    uint8_t stage;
    uint16_t index;
    uint32_t a, b;
    uint64_t c, x;
    void *obj, *obj2;
};

inline Cmd cmd(Op op, uint8_t stage = 0, uint16_t index = 0)
{
    Cmd c = {};
    c.op = op;
    c.stage = stage;
    c.index = index;
    return c;
}

inline bool isPlanDraw(Op op)
{
    return op == OpDraw || op == OpDrawIndexed || op == OpDrawIndirect || op == OpDrawIndexedIndirect ||
           op == OpExecuteCommands || op == OpDrawPatches || op == OpDrawPatchesIndirect;
}
