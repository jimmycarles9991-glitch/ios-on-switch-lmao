// SPDX-License-Identifier: MIT
// Core Animation shaped command buffers on the raw facade (raw_facade_harness.h): passes into a few targets that each pass
// after its target's first loads, each draw a quad of its own vertices, transform and colour by setBytes under a scissor
// of its own, as the render server's scale animations record them (boot 21: 10-16 passes, 67-134 draws a buffer).
#pragma once
#include "raw_facade_harness.h"
#include <cstdint>
#include <string>
#include <vector>

namespace scale_frames {

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

// The process's one raw device (a second open would replace the first's runtime state) and its BGRA8 state
inline const Opened &opened()
{
    static const Opened made = [] {
        const std::string pack = harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm));
        const std::string metallib = harness::writeMetallib();
        id<MTLDevice> device = harness::open(pack);
        if (!device)
            return Opened{};
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
        id<MTLRenderPipelineState> state =
            [device newRenderPipelineStateWithDescriptor:harness::descriptor(library, MTLPixelFormatBGRA8Unorm)
                                                   error:&error];
        return Opened{device, state};
    }();
    return made;
}

struct Shape {
    int passes = 15, draws = 90, targets = 4;
    uint32_t width = 640, height = 360;
};

inline std::vector<id<MTLTexture>> targets(id<MTLDevice> device, const Shape &shape)
{
    std::vector<id<MTLTexture>> made;
    for (int i = 0; i < shape.targets; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:shape.width
                                                                                     height:shape.height
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        made.push_back([device newTextureWithDescriptor:td]);
    }
    return made;
}

// The memoryless colour 1 Core Animation's states draw beside (boot 21's PBGRAXm_A2Xhf: c1 BGRA8, nothing written)
inline id<MTLTexture> scratch(id<MTLDevice> device, const Shape &shape)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:shape.width
                                                                                 height:shape.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeMemoryless;
    td.usage = MTLTextureUsageRenderTarget;
    return [device newTextureWithDescriptor:td];
}

// Pass `p` of `shape`: its encoder begun on `cb`, drawn, ended; with `beside`, that memoryless colour 1 cleared and
// not stored
inline void pass(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> state, const std::vector<id<MTLTexture>> &into,
                 const Shape &shape, int p, uint32_t frame, id<MTLTexture> beside = nil)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = into[size_t(p) % into.size()];
    d.colorAttachments[0].loadAction = p < shape.targets ? MTLLoadActionClear : MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (beside) {
        d.colorAttachments[1].texture = beside;
        d.colorAttachments[1].loadAction = MTLLoadActionClear;
        d.colorAttachments[1].storeAction = MTLStoreActionDontCare;
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    [e setRenderPipelineState:state];
    const int draws = shape.draws / shape.passes + (p < shape.draws % shape.passes ? 1 : 0);
    for (int i = 0; i < draws; i++) {
        const float x = float((i * 37 + p * 11 + frame) % 97) / 97.0f - 0.5f, y = float((i * 13 + p) % 89) / 89.0f;
        const float vertices[16] = {x, y, 0, 1, x + 0.25f, y, 0, 1, x, y + 0.25f, 0, 1, x + 0.25f, y + 0.25f, 0, 1};
        const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(frame % 7) * 0.01f, 0, 0, 1};
        const float colour[4] = {float(i % 5) / 5, float(p % 3) / 3, 0.5f, 1};
        [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
        [e setVertexBytes:transform length:sizeof transform atIndex:2];
        [e setFragmentBytes:colour length:sizeof colour atIndex:0];
        const NSUInteger sx = NSUInteger(i * 17 % 400), sy = NSUInteger(i * 11 % 200);
        [e setScissorRect:MTLScissorRect{sx, sy, 200, 120}];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [e endEncoding];
}

inline id<MTLCommandBuffer> record(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> state,
                                   const std::vector<id<MTLTexture>> &into, const Shape &shape, uint32_t frame)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++)
        pass(cb, state, into, shape, p, frame);
    return cb;
}

// The words of the model's jobs from `first` on, one line a job: its index, word count and the words in hex
inline std::string jobWords(size_t first)
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    std::string out;
    char line[32];
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
        const auto &words = kernel_model::model.jobs[j].words;
        snprintf(line, sizeof line, "job %zu %zu:", j - first, words.size());
        out += line;
        for (uint32_t w : words) {
            snprintf(line, sizeof line, " %08x", w);
            out += line;
        }
        out += "\n";
    }
    return out;
}

}  // namespace scale_frames
