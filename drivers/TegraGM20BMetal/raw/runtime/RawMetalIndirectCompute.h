// SPDX-License-Identifier: MIT
// Indirect command buffers of dispatches (RawMetalIndirectCompute.mm, RawMetalIndirectExecute.mm, raw_icb_compute.h)
#pragma once
#import "RawMetalComputeState.h"
#import "RawMetalIndirect.h"
#include <map>
#include <memory>
#include "raw_compute.h"
#include "raw_icb_compute.h"

using RMKernelSet = raw::ComputeKernelSet<raw::ComputeState>;

// GPU memory a command's regions lie in. A blit's copy of a command shares its source's until either is encoded again.
struct RMICBBlock {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
    uint32_t bytes = 0;
};

// A dispatch command as the CPU encoded it
struct RMICBDispatch {
    RMComputeState *pipeline = nil;
    RMBuffer *buffers[31] = {};
    uint64_t offsets[31] = {};
    std::array<uint32_t, 31> strides = raw::StageIn::unbound();  // setKernelBuffer:offset:attributeStride:atIndex:'s
    std::vector<std::pair<uint32_t, uint32_t>> memory;  // threadgroup memory lengths by index, as the encoder keys them
    std::array<uint64_t, 3> grid{};
    std::array<uint32_t, 3> threads{};
    bool dispatched = false, perThread = false, barrier = false, stageIn = false;
    MTLRegion stageInRegion{};
    std::shared_ptr<RMKernelSet> kernels;  // its own pipeline's at its shape
    std::shared_ptr<RMICBBlock> block;
    raw::icb::ComputeCommand words{};  // what its slot launches
    // a buffer that inherits the encoder's buffers: its descriptors' and stage-in strides' binds (its bind slot's words),
    // the Metal buffer indices they read, those of them with no residual word, and those whose bind stride it reads
    std::vector<raw::icb::ComputeBind> binds;
    std::vector<raw::icb::ComputeStride> strideWords;
    uint32_t bindSlots = 0, alignedSlots = 0, strideSlots = 0;
    bool raw = false;  // its slot launches it (a buffer that inherits no pipeline)
};

// A buffer that inherits the pipeline: its commands written once for one pipeline of the encoder's, as they were at
// `generation`, executed by reference as the buffer's own words are; never changed once made (an execute in flight keeps it)
struct RMICBVariant {
    uint64_t generation = 0;
    raw::Buffer slots, binds;
    std::vector<raw::Buffer> chunks;
    uint64_t chunkUsed = 0;
    uint32_t bindSlots = 0, alignedSlots = 0, strideSlots = 0;
    std::vector<uint8_t> missing;  // a command dispatched that the raw path has no kernel or form for at the pipeline
    ~RMICBVariant();
};

@interface RMIndirectComputeBuffer : RMIndirectCommandBuffer {
@public
    MTLIndirectCommandBufferDescriptor *_descriptor;
    bool _inheritPipeline, _inheritBuffers;
    std::vector<RMICBDispatch> _dispatches;
    std::vector<raw::Buffer> _chunks;  // the blocks' memory, freed with the buffer
    uint64_t _chunkUsed;
    raw::Buffer _binds;    // a buffer that inherits the encoder's buffers: each command's bind slot
    uint64_t _generation;  // bumped by every change of a command
    std::map<RMComputeState *, std::shared_ptr<RMICBVariant>> _variants;  // a buffer that inherits the pipeline
    // every command's bindSlots, alignedSlots and strideSlots, made again at an execute after a command changed
    uint32_t _bindSlots, _alignedSlots, _strideSlots;
    bool _bindsChanged;
    id<MTLIndirectCommandBuffer> _m2v;  // m2v's buffer, into which its replay encodes the commands again
}
@end

@interface RMIndirectComputeCommand : RMObject {
@public
    RMIndirectComputeBuffer *_buffer;
    NSUInteger _index;
}
@end

// A compute encoder's execute of a buffer of dispatches (OpExecuteKernels' object, retained by its command buffer)
@interface RMKernelExecute : NSObject {
@public
    RMIndirectComputeBuffer *_buffer;
    RMBuffer *_range;  // the execution range the GPU reads at _rangeOffset; nil: [_first, _first + _count)
    uint64_t _rangeOffset;
    RMComputeState *_pipeline;  // the encoder's, for a buffer that inherits it
    uint32_t _first, _count;
    // a buffer that inherits the pipeline: each command's kernels at the encoder's (a slot of none launches nothing), made
    // where the buffer had no variant for it at `_kernelsGeneration`, and the variant the execute runs
    std::vector<std::shared_ptr<RMKernelSet>> _kernels;
    uint64_t _kernelsGeneration;
    std::shared_ptr<RMICBVariant> _variant;
}
@end

// RawMetalIndirectCompute.mm
id rmNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count);
// The kernels of `pipeline` for command `c`'s dispatch (null: no raw form), made on the calling thread where no other
// dispatch of the shape made them, and `shape` its regions
std::shared_ptr<RMKernelSet> rmCommandKernels(RMComputeState *pipeline, const RMICBDispatch &c,
                                              raw::ComputeDispatch &shape);
// Command `c`'s regions written into `block` (computeRegionBytes each) with `buffers` bound at `strides`, and the words that
// launch them
bool rmCommandRegions(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBDispatch &c,
                      const raw::ComputeEncoder::IcbBuffer (&buffers)[31], const std::array<uint32_t, 31> &strides,
                      const RMICBBlock &block, raw::icb::ComputeCommand &words, const char **why);
uint32_t rmCommandBytes(const RMKernelSet &kernels);
void rmCommandBuffers(const RMICBDispatch &c, raw::ComputeEncoder::IcbBuffer (&out)[31]);
// Command `c`'s binds of its regions in `block` into c.binds and its masks; false: refused (*why)
bool rmCommandBinds(const RMKernelSet &kernels, const RMICBBlock &block, RMICBDispatch &c, const char **why);
// The bind slot of command `index` of a buffer that inherits the encoder's buffers
uint32_t *rmBindSlot(RMIndirectComputeBuffer *b, NSUInteger index);
// `b`'s commands (under its lock) for `pipeline` with `kernels` (each command's at it), made now; null: refused (*why)
std::shared_ptr<RMICBVariant> rmPipelineVariant(RMIndirectComputeBuffer *b, RMComputeState *pipeline,
                                                const std::vector<std::shared_ptr<RMKernelSet>> &kernels,
                                                const char **why);

// RawMetalCompute.mm: the pipeline the encoder has set
@interface RMComputeEncoder (State)
- (RMComputeState *)rmState;
@end

// RawMetalIndirectExecute.mm: the raw encode of OpExecuteKernels, m2v's replay of it (the commands encoded again into m2v's
// buffer, which executes them), and a blit's reset or copy of a buffer of dispatches (be null: m2v's replay)
bool encodeKernelCommands(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMKernelExecute *x, const char **why);
void replayKernelCommands(id<MTLComputeCommandEncoder> encoder, RMKernelExecute *x, std::set<RMBuffer *> &buffers);
void encodeKernelCommandsBlit(raw::BlitEncoder *be, const Cmd &c);
