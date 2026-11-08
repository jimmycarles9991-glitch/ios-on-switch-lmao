// SPDX-License-Identifier: MIT
// The pack generator's probe of a render state (pack/probe.mm), in process, for a state the pack lacks: the state drawn once with
// probe.mm's tagged inputs and attachments in four m2v command buffers (payloads of 256, 16 and 4096 bytes, and of 256 bound at
// each slot's own offset: PROBE_BIND_OFFSET, raw::probeBindOffset), each captured instead
// of committed (NVK's commands decoded on the CPU, never run: raw_capture.h), and the record the generator makes of them
// (raw_record.h). The twin's program is NVK's own compile of m2v's translation of the function's AIR, the program the raw path
// draws with.
#pragma once
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#include <memory>
#include <string>
#include "raw_compiler.h"
#include "raw_capture.h"
#include "raw_record.h"

namespace raw {

class Probe {
public:
    // `device`: the m2v device the twins are of; `pack`: this build's (its context init and its records' rules)
    Probe(id<MTLDevice> device, const Pack &pack);
    ~Probe();

    // The record of the state `twin` draws, made from `descriptor` (its attachments), whose pack key is `key`
    Compiler::Outcome make(id<MTLRenderPipelineState> twin, MTLRenderPipelineDescriptor *descriptor,
                           const std::string &key, RuntimeRecord &out, std::string &why);

private:
    id<MTLDevice> device_;
    const Pack &pack_;
    id<MTLCommandQueue> queue_;
    NSArray<id<MTLTexture>> *textures_;
    NSArray<id<MTLSamplerState>> *samplers_;
    NSArray<id<MTLBuffer>> *payloads_;  // the bind-offset draw's, one a slot (made at its first)
    IOSurfaceRef framebufferSurface_ = nullptr;
    id<MTLTexture> framebuffer_;
    NSMutableDictionary<NSString *, id<MTLTexture>> *targets_;
    std::unique_ptr<Capture> capture_;
    InitAddresses primedWith_ = {};
    bool primed_ = false;
    id<MTLCommandBuffer> captured_;  // what the capture's memory and programs are read through
    PackRules rules_;
    bool rulesMade_ = false;

    id<MTLTexture> target(MTLPixelFormat format, bool memoryless);
    MTLRenderPassDescriptor *pass(MTLRenderPipelineDescriptor *d);
    Compiler::Outcome run(id<MTLRenderPipelineState> twin, MTLRenderPipelineDescriptor *d, uint32_t bytes,
                          nvdec::ProbeSubmission &out, std::string &why, bool bindOffsets = false);
};

}  // namespace raw
