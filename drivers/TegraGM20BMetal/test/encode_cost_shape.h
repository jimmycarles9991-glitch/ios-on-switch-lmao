// SPDX-License-Identifier: MIT
// hw_test_app's gpu.path.encode_cost command buffer on the facade over the kernel model (scale_frames.h's device
// and BGRA8 state): 6 passes into a 256x256 BGRA8 target, the first clearing it and the rest loading it, each beside a
// memoryless RGBA16Float colour 1 cleared and not stored, each of a render pass descriptor made for it; each draw bound
// as CAState.bind binds it (a strip of four 48-byte vertices at vertex buffer 1, the identity transform at 2, 256 zero
// bytes at vertex 0 and 3 and fragment 0 to 3, all by setBytes) under a scissor of its own.
#pragma once
#include "scale_frames.h"
#include <dispatch/dispatch.h>
#include <map>
#include <string>

// Signals once the object it is associated with is destroyed
@interface EncodeCostGone : NSObject
@property (nonatomic, strong) dispatch_semaphore_t gone;
@end

@implementation EncodeCostGone

- (void)dealloc
{
    dispatch_semaphore_signal(_gone);
}

@end

@interface NSObject (EncodeCostShape)
- (NSDictionary<NSString *, NSNumber *> *)switchMetalEncodeProfile;  // TegraGM20BMetalEncodeCounters.mm (debug builds)
- (NSString *)switchMetalPath;                                       // TegraGM20BMetalCommandBuffer.mm
@end

namespace encode_cost {

constexpr int kBuffers = 24, kPasses = 6, kSize = 256;

struct Vertex {
    float position[4], texcoord0[2], texcoord1[2];
    __fp16 color[4], pad[4];
};

static_assert(sizeof(Vertex) == 48);

struct Bench {
    id<MTLCommandQueue> queue;
    id<MTLTexture> target, scratch;
};

inline const Bench &bench()
{
    static const Bench made = [] {
        id<MTLDevice> device = scale_frames::opened().device;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:kSize
                                                                                     height:kSize
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        td.pixelFormat = MTLPixelFormatRGBA16Float;
        td.storageMode = MTLStorageModeMemoryless;
        td.usage = MTLTextureUsageRenderTarget;
        return Bench{[device newCommandQueue], target, [device newTextureWithDescriptor:td]};
    }();
    return made;
}

inline void encode(id<MTLCommandBuffer> cb, int draws)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const uint8_t zeros[256] = {};
    for (int p = 0; p < kPasses; p++) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor new];
        d.colorAttachments[0].texture = bench().target;
        d.colorAttachments[0].loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[1].texture = bench().scratch;
        d.colorAttachments[1].loadAction = MTLLoadActionClear;
        d.colorAttachments[1].storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
        [e setRenderPipelineState:scale_frames::opened().state];
        for (int i = 0; i < draws; i++) {
            const float x = float(i % 8) / 4 - 1, y = float(p) / 3 - 1;
            const __fp16 c[4] = {__fp16(float(i % 4) / 4), __fp16(float(p) / 8), 0.5, 1};
            const Vertex v[4] = {{{x, y, 0, 1}, {0, 1}, {0, 1}, {c[0], c[1], c[2], c[3]}, {}},
                                 {{x + 0.25f, y, 0, 1}, {1, 1}, {1, 1}, {c[0], c[1], c[2], c[3]}, {}},
                                 {{x, y + 0.3f, 0, 1}, {0, 0}, {0, 0}, {c[0], c[1], c[2], c[3]}, {}},
                                 {{x + 0.25f, y + 0.3f, 0, 1}, {1, 0}, {1, 0}, {c[0], c[1], c[2], c[3]}, {}}};
            [e setVertexBytes:v length:sizeof v atIndex:1];
            [e setVertexBytes:identity length:sizeof identity atIndex:2];
            for (NSUInteger slot : {0, 3})
                [e setVertexBytes:zeros length:sizeof zeros atIndex:slot];
            for (NSUInteger slot = 0; slot < 4; slot++)
                [e setFragmentBytes:zeros length:sizeof zeros atIndex:slot];
            [e setScissorRect:MTLScissorRect{NSUInteger(i % 8) * 32, NSUInteger(p) * 32, 32, 64}];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
    }
}

inline id<MTLCommandBuffer> commitOne(int draws)
{
    id<MTLCommandBuffer> cb;
    @autoreleasepool {
        cb = [bench().queue commandBuffer];
        encode(cb, draws);
        [cb commit];
    }
    return cb;
}

inline NSArray<id<MTLCommandBuffer>> *batch(int draws, int count = kBuffers)
{
    NSMutableArray<id<MTLCommandBuffer>> *made = [NSMutableArray arrayWithCapacity:NSUInteger(count)];
    for (int i = 0; i < count; i++)
        [made addObject:commitOne(draws)];
    return made;
}

// As hw_test_app's Swift records a batch: outside any autorelease pool of its own, into an array no pool holds
inline NSArray<id<MTLCommandBuffer>> *batchUnpooled(int draws)
{
    NSMutableArray<id<MTLCommandBuffer>> *made = [[NSMutableArray alloc] initWithCapacity:kBuffers];
    for (int i = 0; i < kBuffers; i++) {
        id<MTLCommandBuffer> cb = [bench().queue commandBuffer];
        encode(cb, draws);
        [cb commit];
        [made addObject:cb];
    }
    return made;
}

// Each completed; the command buffers by switchMetalPath ("failed" for one that did not complete)
inline std::map<std::string, unsigned> finish(NSArray<id<MTLCommandBuffer>> *buffers)
{
    std::map<std::string, unsigned> paths;
    for (id<MTLCommandBuffer> cb in buffers) {
        [cb waitUntilCompleted];
        paths[cb.status == MTLCommandBufferStatusCompleted ? [(id)cb switchMetalPath].UTF8String : "failed"]++;
    }
    return paths;
}

// `count` command buffers at `draws` committed and completed, then destroyed: their recordings back with the runtime
// (a command buffer's last reference can be the completion thread's, which it drops after waitUntilCompleted returns).
// False where one did not complete on the driver or outlived its completion by 5 s.
template <typename Recorded> bool settle(int draws, int count, Recorded recorded)
{
    static char goneKey;
    dispatch_semaphore_t gone = dispatch_semaphore_create(0);
    bool raw = true;
    @autoreleasepool {
        NSArray<id<MTLCommandBuffer>> *done = batch(draws, count);
        recorded();
        const std::map<std::string, unsigned> paths = finish(done);
        raw = paths.size() == 1 && paths.begin()->first == "gm20b";
        for (id<MTLCommandBuffer> cb in done) {
            EncodeCostGone *sentinel = [EncodeCostGone new];
            sentinel.gone = gone;
            objc_setAssociatedObject(cb, &goneKey, sentinel, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
    }
    for (int i = 0; i < count; i++)
        if (dispatch_semaphore_wait(gone, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
            return false;
    return raw;
}

inline bool settle(int draws, int count = kBuffers)
{
    return settle(draws, count, [] {});
}

inline NSDictionary<NSString *, NSNumber *> *profile()
{
    return [(id)scale_frames::opened().device switchMetalEncodeProfile];
}

}  // namespace encode_cost
