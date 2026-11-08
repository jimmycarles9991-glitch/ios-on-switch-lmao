// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): every selector Core Animation's QuartzCore (21H461) references that is a method of a command buffer,
// queue or encoder protocol (MTL*CommandBuffer, MTL*CommandEncoder, MTL*CommandQueue and their SPI; the dyld cache's selector
// references of QuartzCore intersected with the protocols' methods) must be one the facade answers itself, or one it hands to m2v
// on purpose (compute and tile shaders, which m2v answers or refuses as today). A selector that reaches forwardInvocation: sends
// the whole command buffer to m2v (r7l: setProtectionOptions:, on every one). Prints each and exits 1 on any unexpected.
// With a pack, also the pool's growth: 48 MiB of 720p render targets made through the facade, a millisecond apart, must find
// room every time (the grower thread made the chunks once the free bytes fell below the low mark): the rates line's grow= is
// the count of allocations that had to grow the pool themselves, and must be 0.
// usage: selcheck [pack]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include "../runtime/RawMetal.h"

int main(int argc, char **argv)
{
    @autoreleasepool {
        struct Use {
            const char *cls, *sel;
        };

        // QuartzCore's references by the object they go to (raw class); "-" marks the ones m2v keeps on purpose
        static const Use uses[] = {
            {"RMCommandBuffer", "GPUEndTime"},
            {"RMCommandBuffer", "GPUStartTime"},
            {"RMCommandBuffer", "addCompletedHandler:"},
            {"RMCommandBuffer", "addScheduledHandler:"},
            {"RMCommandBuffer", "blitCommandEncoder"},
            {"RMCommandBuffer", "commit"},
            {"RMCommandBuffer", "commitAndWaitUntilSubmitted"},
            {"RMCommandBuffer", "computeCommandEncoderWithDispatchType:"},
            {"RMCommandBuffer", "encodeCacheHintFinalize:resourceGroups:count:"},
            {"RMCommandBuffer", "encodeCacheHintTag:resourceGroups:count:"},
            {"RMCommandBuffer", "encodeSignalEvent:value:"},
            {"RMCommandBuffer", "encodeWaitForEvent:value:"},
            {"RMCommandBuffer", "enqueue"},
            {"RMCommandBuffer", "error"},
            {"RMCommandBuffer", "kernelEndTime"},
            {"RMCommandBuffer", "kernelStartTime"},
            {"RMCommandBuffer", "label"},
            {"RMCommandBuffer", "setLabel:"},
            {"RMCommandBuffer", "popDebugGroup"},
            {"RMCommandBuffer", "pushDebugGroup:"},
            {"RMCommandBuffer", "protectionOptions"},
            {"RMCommandBuffer", "setProtectionOptions:"},
            {"RMCommandBuffer", "renderCommandEncoderWithDescriptor:"},
            {"RMCommandBuffer", "status"},
            {"RMCommandBuffer", "waitUntilCompleted"},
            {"RMCommandBuffer", "waitUntilScheduled"},
            {"RMCommandBuffer", "device"},
            {"RMQueue", "commandBuffer"},
            {"RMQueue", "device"},
            {"RMQueue", "label"},
            {"RMQueue", "setLabel:"},
            {"RMQueue", "setCompletionQueue:"},
            {"RMRenderEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"},
            {"RMRenderEncoder", "drawPrimitives:vertexStart:vertexCount:instanceCount:"},
            {"RMRenderEncoder", "endEncoding"},
            {"RMRenderEncoder", "insertDebugSignpost:"},
            {"RMRenderEncoder", "popDebugGroup"},
            {"RMRenderEncoder", "pushDebugGroup:"},
            {"RMRenderEncoder", "setLabel:"},
            {"RMRenderEncoder", "label"},
            {"RMRenderEncoder", "device"},
            {"RMRenderEncoder", "setBlendColorRed:green:blue:alpha:"},
            {"RMRenderEncoder", "setCullMode:"},
            {"RMRenderEncoder", "setDepthBias:slopeScale:clamp:"},
            {"RMRenderEncoder", "setDepthStencilState:"},
            {"RMRenderEncoder", "setFragmentBuffer:offset:atIndex:"},
            {"RMRenderEncoder", "setFragmentBufferOffset:atIndex:"},
            {"RMRenderEncoder", "setFragmentSamplerState:atIndex:"},
            {"RMRenderEncoder", "setFragmentTexture:atIndex:"},
            {"RMRenderEncoder", "setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:"},
            {"RMRenderEncoder", "setFrontFacingWinding:"},
            {"RMRenderEncoder", "setLineWidth:"},
            {"RMRenderEncoder", "setRenderPipelineState:"},
            {"RMRenderEncoder", "setScissorRect:"},
            {"RMRenderEncoder", "setTriangleFillMode:"},
            {"RMRenderEncoder", "setVertexBuffer:offset:atIndex:"},
            {"RMRenderEncoder", "setVertexBufferOffset:atIndex:"},
            {"RMRenderEncoder", "setViewport:"},
            {"RMRenderEncoder", "setVisibilityResultMode:offset:"},
            {"RMRenderEncoder", "tileWidth"},
            {"RMRenderEncoder", "tileHeight"},
            {"-", "dispatchThreadsPerTile:"},
            {"-", "dispatchThreadsPerTile:inRegion:"},
            {"-", "setTileBytes:length:atIndex:"},
            {"-", "setTileTexture:atIndex:"},
            {"RMBlitEncoder", "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"},
            {"RMBlitEncoder", "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:"
                              "destinationSlice:destinationLevel:"
                              "destinationOrigin:"},
            {"RMBlitEncoder", "generateMipmapsForTexture:"},
            {"RMBlitEncoder", "endEncoding"},
            {"-", "dispatchThreadgroups:threadsPerThreadgroup:"},
            {"-", "setBuffer:offset:atIndex:"},
            {"-", "setComputePipelineState:"},
            {"-", "setTexture:atIndex:"},
            {"-", "setThreadgroupMemoryLength:atIndex:"},
            // resources: answered without making an m2v twin (forwarding one would make it)
            {"RMTexture", "setOwnerWithIdentity:"},
            {"RMBuffer", "setOwnerWithIdentity:"},
            {"RMTexture", "protectionOptions"},
            {"RMTexture", "rotation"},
            {"RMBuffer", "newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:"},
            {"RMTexture", "waitUntilComplete"},
            {"RMBuffer", "waitUntilComplete"},
            {"RMTexture", "compressionType"},
            {"RMTexture", "isCompressed"},
            {"RMTexture", "isDrawable"},
            {"RMTexture", "swizzle"},
            {"RMTexture", "heapOffset"},
            {"RMTexture", "newTextureViewWithPixelFormat:textureType:levels:slices:"},
            {"RMBuffer", "heapOffset"},
            {"RMBlitEncoder", "fillBuffer:range:value:"},
            {"RMCommandBuffer", "userDictionary"},
            {"RMCommandBuffer", "globalTraceObjectID"},
            {"RMSampler", "pixelFormat"},
            {"RMPipeline", "maxTotalThreadsPerThreadgroup"},
            // residency sets (iOS 18 on, Core Animation 27's): everything is resident on the raw path; a set's allocatedSize
            // asks each allocation's
            {"RMCommandBuffer", "useResidencySet:"},
            {"RMCommandBuffer", "useResidencySets:count:"},
            {"RMCommandBuffer", "setResponsibleTaskIDs:count:"},
            {"RMTexture", "allocatedSize"},
            {"RMQueue", "addResidencySet:"},
            {"RMQueue", "addResidencySets:count:"},
            {"RMQueue", "removeResidencySet:"},
            {"RMQueue", "removeResidencySets:count:"},
            {"RMRenderEncoder", "useResidencySet:"},
            {"RMRenderEncoder", "useResidencySets:count:"},
            {"RMBlitEncoder", "useResidencySet:"},
        };
        int bad = 0, own = 0, m2v = 0;
        for (const Use &u : uses) {
            if (!strcmp(u.cls, "-")) {
                m2v++;
                continue;
            }
            Class c = NSClassFromString(@(u.cls));
            if (!c || !class_getInstanceMethod(c, sel_registerName(u.sel))) {
                printf("NOT ANSWERED -[%s %s] (it would send the command buffer to m2v, or make a twin)\n", u.cls,
                       u.sel);
                bad++;
            } else {
                own++;
            }
        }
        printf("selcheck: %d answered by the facade, %d left to m2v on purpose, %d not answered\n", own, m2v, bad);
        (void)raw_metal_pipeline_key;
        if (argc > 1) {
            NSString *why = nil;
            id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, nil, &why);
            if (!dev) {
                printf("NO RAW DEVICE: %s\n", why.UTF8String);
                return 1;
            }
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:720
                                                                                        height:1280
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            std::vector<id<MTLTexture>> keep;
            for (int i = 0; i < 13; i++) {  // 3.5 MiB each
                keep.push_back([dev newTextureWithDescriptor:d]);
                usleep(getenv("SELCHECK_GAP_US") ? (useconds_t)atoi(getenv("SELCHECK_GAP_US")) : 1000);
            }
            id<MTLCommandQueue> q = [dev newCommandQueue];
            // a command buffer with residency sets on it and its queue stays raw (m2v=0 below)
            NSObject *set = [NSObject new];
            ((void (*)(id, SEL, id))objc_msgSend)(q, sel_registerName("addResidencySet:"), set);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            ((void (*)(id, SEL, id))objc_msgSend)(cb, sel_registerName("useResidencySet:"), set);
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            ((void (*)(id, SEL, id))objc_msgSend)(b, sel_registerName("useResidencySet:"), set);
            [b copyFromTexture:keep[0]
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(16, 16, 1)
                        toTexture:keep[1]
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            char line[1024];
            if (raw_metal_rates(line, sizeof line))
                printf("rates: %s\n", line);
            const char *g = strstr(line, " grow=");
            const int stalls = g ? atoi(g + 6) : -1;
            printf("pool growth on the allocating thread: %d\n", stalls);
            if (stalls != 0)
                bad++;
            if (!strstr(line, " m2v=0 ")) {
                printf("RESIDENCY: a command buffer with residency sets went to m2v\n");
                bad++;
            }
        }
        return bad ? 1 : 0;
    }
}
