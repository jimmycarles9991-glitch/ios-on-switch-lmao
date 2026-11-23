// SPDX-License-Identifier: MIT
// Indirect command buffers of dispatches (TegraGM20BMetalIndirectCompute.mm, TegraGM20BMetalIndirectComputeKernels.mm,
// TegraGM20BMetalIndirectComputeSlots.mm, TegraGM20BMetalIndirectExecute.mm, icb_compute.h)
#pragma once
#import "TegraGM20BMetalComputeState.h"
#import "TegraGM20BMetalIndirect.h"
#include <map>
#include <memory>
#include "compute.h"
#include "icb_compute.h"

using SwitchMetalKernelSet = gm20b::ComputeKernelSet<gm20b::ComputeState>;

// GPU memory a command's regions lie in. A blit's copy of a command shares its source's until either is encoded again.
struct SwitchMetalICBBlock {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
    uint32_t bytes = 0;
};

// Why a command that dispatches launches nothing of its own (`why` null: it launches, or its grid is empty), and what
// that is to the command buffer whose execute reaches it
struct SwitchMetalICBUnmade {
    const char *why = nullptr;
    gm20b::ComputeFailure failure = gm20b::ComputeFailure::Internal;
    SwitchMetalPred pred = PredComputeKernel;  // what the execute that reaches it fails as
};

// A dispatch command as the CPU encoded it
struct SwitchMetalICBDispatch {
    SwitchMetalComputeState *pipeline = nil;
    SwitchMetalBuffer *buffers[31] = {};
    uint64_t offsets[31] = {};
    std::array<uint32_t, 31> strides = gm20b::StageIn::unbound();  // setKernelBuffer:offset:attributeStride:atIndex:'s
    std::vector<std::pair<uint32_t, uint32_t>> memory;  // threadgroup memory lengths by index, as the encoder keys them
    std::array<uint64_t, 3> grid{};
    std::array<uint32_t, 3> threads{};
    bool dispatched = false, perThread = false, barrier = false, stageIn = false;
    MTLRegion stageInRegion{};
    std::shared_ptr<SwitchMetalKernelSet> kernels;  // its own pipeline's at its shape
    std::shared_ptr<SwitchMetalICBBlock> block;
    gm20b::icb::ComputeCommand words{};  // what its slot launches
    // a buffer that inherits the encoder's buffers: its descriptors', stage-in strides' and root table entries' binds
    // (its bind slot's words), the Metal buffer indices they read, and those whose bind stride it reads
    std::vector<gm20b::icb::ComputeBind> binds;
    std::vector<gm20b::icb::ComputeStride> strideWords;
    std::vector<gm20b::icb::ComputeRoot> roots;
    uint32_t bindSlots = 0, strideSlots = 0;
    bool raw = false;  // its slot launches it (a buffer that inherits no pipeline)
    SwitchMetalICBUnmade unmade;
    // what the CPU encoded that Metal's validation stops and that has no form: an object of another device, or what its
    // buffer inherits; it launches nothing and an execute that reaches it fails InvalidResource, until a reset of it
    SwitchMetalPred invalid = PredNone;
    const char *invalidWhy = nullptr;
};

// A buffer that inherits the pipeline: its commands written once for one pipeline of the encoder's, as they were at
// `generation`, executed by reference as the buffer's own words are; never changed once made (an execute in flight
// keeps it)
struct SwitchMetalICBVariant {
    uint64_t generation = 0;
    gm20b::Buffer slots, binds;
    uint32_t slotWords = gm20b::icb::kComputeSlotWords, bindWords = gm20b::icb::kComputeBindSlotWords;
    std::vector<gm20b::Buffer> chunks;
    uint64_t chunkUsed = 0;
    uint32_t bindSlots = 0, strideSlots = 0;
    std::vector<SwitchMetalICBUnmade> missing;  // each command's at the pipeline
    ~SwitchMetalICBVariant();
};

// _raw holds each command's slot of _slotWords, gm20b::icb::computeSlotWords of the most a command launches
@interface SwitchMetalIndirectComputeBuffer : SwitchMetalIndirectCommandBuffer {
@public
    MTLIndirectCommandBufferDescriptor *_descriptor;
    bool _inheritPipeline, _inheritBuffers;
    std::vector<SwitchMetalICBDispatch> _dispatches;
    std::vector<gm20b::Buffer> _chunks;  // the blocks' memory, freed with the buffer
    uint64_t _chunkUsed;
    gm20b::Buffer _binds;  // a buffer that inherits the encoder's buffers: each command's bind slot of _bindWords
    uint32_t _bindWords;   // gm20b::icb::computeBindSlotWords of the most a command binds
    // the slots and bind slots before they grew, which submitted executes and copies still name: freed with the buffer
    std::vector<gm20b::Buffer> _retired;
    uint64_t _generation;  // bumped by every change of a command
    std::map<SwitchMetalComputeState *, std::shared_ptr<SwitchMetalICBVariant>>
        _variants;  // a buffer that inherits the pipeline
    // every command's bindSlots and strideSlots, made again at an execute after a command changed
    uint32_t _bindSlots, _strideSlots;
    bool _bindsChanged;
}
@end

@interface SwitchMetalIndirectComputeCommand : SwitchMetalObject {
@public
    SwitchMetalIndirectComputeBuffer *_buffer;
    NSUInteger _index;
}
@end

// A compute encoder's execute of a buffer of dispatches (OpExecuteKernels' object, retained by its command buffer)
@interface SwitchMetalKernelExecute : NSObject {
@public
    SwitchMetalIndirectComputeBuffer *_buffer;
    SwitchMetalBuffer *_range;  // the execution range the GPU reads at _rangeOffset; nil: [_first, _first + _count)
    uint64_t _rangeOffset;
    SwitchMetalComputeState *_pipeline;  // the encoder's, for a buffer that inherits it
    uint32_t _first, _count;
    // a buffer that inherits the pipeline: each command's kernels at the encoder's (a slot of none launches nothing)
    // and why one that dispatches has none, made where the buffer had no variant for it at `_kernelsGeneration`, and
    // the variant the execute runs
    std::vector<std::shared_ptr<SwitchMetalKernelSet>> _kernels;
    std::vector<SwitchMetalICBUnmade> _unmade;
    uint64_t _kernelsGeneration;
    std::shared_ptr<SwitchMetalICBVariant> _variant;
}
@end

// TegraGM20BMetalIndirectCompute.mm
id gm20bNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count);
// Command `index`'s words written again from what the CPU encoded, under the buffer's lock
void gm20bWriteCommand(SwitchMetalIndirectComputeBuffer *b, NSUInteger index);

// TegraGM20BMetalIndirectComputeKernels.mm
// The kernels of `pipeline` for command `c`'s dispatch, made on the calling thread where no other dispatch of the shape
// made them, and `shape` its regions; null where it launches nothing: `unmade` says why, or nothing for an empty grid
std::shared_ptr<SwitchMetalKernelSet> gm20bCommandKernels(SwitchMetalComputeState *pipeline,
                                                          const SwitchMetalICBDispatch &c,
                                                          gm20b::ComputeDispatch &shape, SwitchMetalICBUnmade &unmade);
// Command `c`'s regions written into `block` (computeRegionBytes each) with `buffers` bound at `strides`, and the words
// that launch them
bool gm20bCommandRegions(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape,
                         const SwitchMetalICBDispatch &c, const gm20b::ComputeEncoder::IcbBuffer (&buffers)[31],
                         const std::array<uint32_t, 31> &strides, const SwitchMetalICBBlock &block,
                         gm20b::icb::ComputeCommand &words, const char **why);
// The bytes of a command's regions: past UINT32_MAX, more than X1's 4 GB of memory holds
uint64_t gm20bCommandBytes(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape);
void gm20bCommandBuffers(const SwitchMetalICBDispatch &c, gm20b::ComputeEncoder::IcbBuffer (&out)[31]);
// Command `c`'s binds of its regions in `block` into c.binds and its masks; false: refused (*why)
bool gm20bCommandBinds(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape,
                       const SwitchMetalICBBlock &block, SwitchMetalICBDispatch &c, const char **why);

// TegraGM20BMetalIndirectComputeSlots.mm
// A block of `bytes` for a command's regions in `b`'s chunks; null: no memory for it
std::shared_ptr<SwitchMetalICBBlock> gm20bNewBlock(SwitchMetalIndirectComputeBuffer *b, uint32_t bytes);
// The slot and bind slot sizes command `c` needs
uint64_t gm20bSlotWords(const SwitchMetalICBDispatch &c);
uint64_t gm20bBindWords(const SwitchMetalICBDispatch &c);
uint32_t *gm20bComputeSlot(SwitchMetalIndirectComputeBuffer *b, NSUInteger index);
// The bind slot of command `index` of a buffer that inherits the encoder's buffers
uint32_t *gm20bBindSlot(SwitchMetalIndirectComputeBuffer *b, NSUInteger index);
// Command `index`'s slot and bind slot as the CPU keeps it (under the buffer's lock)
void gm20bWriteWords(SwitchMetalIndirectComputeBuffer *b, NSUInteger index);
// `b`'s slots at least `slotWords` and its bind slots at least `bindWords` long (under its lock), each command written
// again where they grew; false: no memory for them
bool gm20bFitSlots(SwitchMetalIndirectComputeBuffer *b, uint64_t slotWords, uint64_t bindWords);
// `b`'s commands (under its lock) for `pipeline` with `kernels` (each command's at it) and why each that dispatches has
// none (`unmade`), made now; null: no memory for them (*why)
std::shared_ptr<SwitchMetalICBVariant>
gm20bPipelineVariant(SwitchMetalIndirectComputeBuffer *b, SwitchMetalComputeState *pipeline,
                     const std::vector<std::shared_ptr<SwitchMetalKernelSet>> &kernels,
                     const std::vector<SwitchMetalICBUnmade> &unmade, const char **why);

// TegraGM20BMetalCompute.mm: the pipeline the encoder has set
@interface SwitchMetalComputeEncoder (State)
- (SwitchMetalComputeState *)gm20bState;
@end

// TegraGM20BMetalCompute.mm: a compute call or command that fails its command buffer with Metal's validation error, its
// out-of-memory error or the driver's own (`failure`), named by `pred` with `why` in the predicate's first line. A call
// that fails records nothing of itself.
void gm20bComputeFails(SwitchMetalCommandBuffer *cb, gm20b::ComputeFailure failure, SwitchMetalPred pred, uint64_t a,
                       const char *why);
// TegraGM20BMetalComputeAdmission.mm: whether a predication Start or EndDoWhile naming no buffer withholds the dispatches of
// the compute unit holding plan position `at`, whose encoders are `serial`
bool gm20bComputeUnitWithheld(SwitchMetalCommandBuffer *cb, size_t at, bool serial);

// TegraGM20BMetalIndirectExecute.mm: the raw encode of OpExecuteKernels, and a blit's reset or copy of a buffer of dispatches
bool encodeKernelCommands(gm20b::ComputeEncoder &e, gm20b::CommandBuffer &rc, SwitchMetalKernelExecute *x,
                          const char **why);
void encodeKernelCommandsBlit(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const Cmd &c);
