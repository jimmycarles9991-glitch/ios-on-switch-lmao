// SPDX-License-Identifier: MIT
// raw GM20B path: stored depth/stencil render targets through the facade (HW Test's metal.render.depth_stencil), over
// the mock GPU (NVDEC_WRITETHROUGH, whose copy engine runs and whose 3D engine does not). Each is made in the raw path's own zeta
// memory with no twin; m2v's twin, asked for as a replay asks, is NVK's image, the texels moved into it and the raw path over it
// from then on. Interleaved: the raw path writes texture A's depth and stencil from buffers
// (MTLBlitOptionDepthFromDepthStencil, StencilFromDepthStencil), m2v copies A into B, the raw path reads B's back; then the
// raw path writes B, m2v copies it into A, the raw path reads A: each driver sees the texels the other wrote. A pass that loads
// and stores one takes the raw path, and, given HW Test's shader library, its test's pass (three quads under depth and stencil
// states, its own functions made at their states' creation) too, and so do gpu.clear.formats' sampled Depth32Float,
// Depth16Unorm and Stencil8 targets' clears and readbacks.
// usage: zetaframe <pack> <cache> <nvk dylib> [HW Test's metallib]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include "../runtime/RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static uint8_t pattern(size_t i)
{
    return uint8_t(i * 131u + (i >> 7) * 17u + 5u);
}

static NSString *path(id<MTLCommandBuffer> cb)
{
    const SEL sel = NSSelectorFromString(@"switchRawPath");
    return [(id)cb respondsToSelector:sel] ? ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel) : @"device";
}

static id<MTLDepthStencilState> depthState(id<MTLDevice> dev, bool write, MTLCompareFunction stencil, bool replace)
{
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionLess;
    d.depthWriteEnabled = write;
    MTLStencilDescriptor *face = [MTLStencilDescriptor new];
    face.stencilCompareFunction = stencil;
    face.depthStencilPassOperation = replace ? MTLStencilOperationReplace : MTLStencilOperationKeep;
    d.frontFaceStencil = face;
    d.backFaceStencil = face;
    return [dev newDepthStencilStateWithDescriptor:d];
}

// HW Test's metal.render.depth_stencil pass: 1 where it did not take the raw path
static int hwTestPass(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> depth, const char *library)
{
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(library)] error:&error];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"quad_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"solid_fragment"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> state = lib ? [dev newRenderPipelineStateWithDescriptor:d error:&error] : nil;
    MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:depth.width
                                                                                 height:depth.height
                                                                              mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [dev newTextureWithDescriptor:cd];
    if (!state || !color) {
        printf("FAIL HW Test's state: %s\n", error.localizedDescription.UTF8String ?: "no texture");
        return 1;
    }
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = color;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.depthAttachment.texture = depth;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.clearDepth = 1;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.stencilAttachment.texture = depth;
    p.stencilAttachment.loadAction = MTLLoadActionClear;
    p.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:state];
    [e setStencilReferenceValue:1];

    const struct {
        float z;
        id<MTLDepthStencilState> ds;
    } draws[] = {{0.5f, depthState(dev, true, MTLCompareFunctionAlways, true)},
                 {0.7f, depthState(dev, true, MTLCompareFunctionAlways, false)},
                 {0.1f, depthState(dev, false, MTLCompareFunctionEqual, false)}};

    for (const auto &draw : draws) {
        const float corners[16] = {-1, -1, draw.z, 1, 1, -1, draw.z, 1, -1, 1, draw.z, 1, 1, 1, draw.z, 1};
        const float rgba[4] = {1, 0, 0, 1};
        [e setDepthStencilState:draw.ds];
        [e setVertexBytes:corners length:sizeof corners atIndex:0];
        [e setFragmentBytes:rgba length:sizeof rgba atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const bool raw = [path(cb) isEqualToString:@"raw"];
    printf("%s HW Test's pass: three quads under its depth and stencil states, path %s\n", raw ? "OK  " : "FAIL",
           path(cb).UTF8String);
    return raw ? 0 : 1;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: zetaframe <pack> <cache> <nvk dylib> [HW Test's metallib]\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s %s\n", nmb_last_error(), why.UTF8String);
            return 1;
        }
        raw_metal_complete_states(argv[2]);
        id<MTLCommandQueue> q = [dev newCommandQueue], m2vQueue = [m2v newCommandQueue];
        int failed = 0;
        auto expect = [&](bool ok, const char *what, NSString *took) {
            printf("%s %s%s%s\n", ok ? "OK  " : "FAIL", what, took ? ", path " : "", took ? took.UTF8String : "");
            failed += !ok;
        };
        const unsigned w = 64, h = 64;
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> textures[2] = {[dev newTextureWithDescriptor:d], [dev newTextureWithDescriptor:d]};
        for (id<MTLTexture> t : textures) {
            RMTexture *x = (RMTexture *)t;
            const bool owned = [(id)t isKindOfClass:[RMTexture class]] && x->_rawOk && x->_raw.zeta &&
                               !x->_raw.memoryless && !x->_twin;
            expect(owned, "a stored Depth32Float_Stencil8 render target is the raw path's own, with no twin", nil);
            struct switch_nvk_image_memory m = {};
            const bool moved = owned && [x rmTwin] && native_bridge_metal_texture_memory(x->_twin, &m) == 0 &&
                               x->_raw.va == m.addr && x->_raw.pitch == m.row_stride_B &&
                               x->_raw.blockHeight == m.y_log2;
            expect(moved,
                   "its twin, asked for as a replay asks, is NVK's image, which the raw path is over from then on",
                   nil);
            if (!moved)
                return 1;
        }

        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.depthAttachment.texture = textures[0];
        p.depthAttachment.loadAction = MTLLoadActionLoad;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        p.stencilAttachment.texture = textures[0];
        p.stencilAttachment.loadAction = MTLLoadActionLoad;
        p.stencilAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        expect([path(cb) isEqualToString:@"raw"], "a pass that loads and stores it", path(cb));
        if (argc > 4)
            failed += hwTestPass(dev, q, textures[0], argv[4]);

        id<MTLBuffer> depth = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> stencil = [dev newBufferWithLength:w * h options:MTLResourceStorageModeShared];
        auto blit = [&](id<MTLTexture> t, bool into) {
            id<MTLCommandBuffer> c = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
            for (int aspect = 0; aspect < 2; aspect++) {
                id<MTLBuffer> buffer = aspect ? stencil : depth;
                const NSUInteger unit = aspect ? 1 : 4;
                const MTLBlitOption option =
                    aspect ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
                if (into)
                    [e copyFromBuffer:buffer
                               sourceOffset:0
                          sourceBytesPerRow:w * unit
                        sourceBytesPerImage:w * h * unit
                                 sourceSize:MTLSizeMake(w, h, 1)
                                  toTexture:t
                           destinationSlice:0
                           destinationLevel:0
                          destinationOrigin:MTLOriginMake(0, 0, 0)
                                    options:option];
                else
                    [e copyFromTexture:t
                                     sourceSlice:0
                                     sourceLevel:0
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:MTLSizeMake(w, h, 1)
                                        toBuffer:buffer
                               destinationOffset:0
                          destinationBytesPerRow:w * unit
                        destinationBytesPerImage:w * h * unit
                                         options:option];
            }
            [e endEncoding];
            [c commit];
            [c waitUntilCompleted];
            return path(c);
        };
        auto m2vCopy = [&](id<MTLTexture> from, id<MTLTexture> to) {
            id<MTLCommandBuffer> c = [m2vQueue commandBuffer];
            id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
            [e copyFromTexture:((RMTexture *)from)->_twin toTexture:((RMTexture *)to)->_twin];
            [e endEncoding];
            [c commit];
            [c waitUntilCompleted];
            return c.status == MTLCommandBufferStatusCompleted;
        };
        for (int round = 0; round < 2; round++) {
            id<MTLTexture> written = textures[round], read = textures[1 - round];
            std::vector<uint8_t> wantDepth(w * h * 4), wantStencil(w * h);
            for (size_t i = 0; i < wantDepth.size(); i++)
                wantDepth[i] = ((uint8_t *)depth.contents)[i] = pattern(i + 999 * (round + 1));
            for (size_t i = 0; i < wantStencil.size(); i++)
                wantStencil[i] = ((uint8_t *)stencil.contents)[i] = pattern(i + 4242 * (round + 1));
            NSString *in = blit(written, true);
            const bool copied = m2vCopy(written, read);
            memset(depth.contents, 0xee, depth.length);
            memset(stencil.contents, 0xee, stencil.length);
            NSString *out = blit(read, false);
            const bool same = !memcmp(depth.contents, wantDepth.data(), wantDepth.size()) &&
                              !memcmp(stencil.contents, wantStencil.data(), wantStencil.size());
            char what[160];
            snprintf(what, sizeof what,
                     "raw writes %c's depth and stencil, m2v copies %c into %c, raw reads %c's: the same texels",
                     'A' + round, 'A' + round, 'B' - round, 'B' - round);
            expect([in isEqualToString:@"raw"] && copied && [out isEqualToString:@"raw"] && same, what,
                   [NSString stringWithFormat:@"%@ / m2v / %@", in, out]);
        }
        for (MTLPixelFormat f : {MTLPixelFormatDepth32Float, MTLPixelFormatDepth16Unorm, MTLPixelFormatStencil8}) {
            // HW Test's gpu.clear.formats targets, which may also be sampled: a clear pass, a readback with no options
            d.pixelFormat = f;
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            RMTexture *t = (RMTexture *)[dev newTextureWithDescriptor:d];
            MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLRenderPassAttachmentDescriptor *aspect =
                f == MTLPixelFormatStencil8 ? clear.stencilAttachment : clear.depthAttachment;
            aspect.texture = t;
            aspect.loadAction = MTLLoadActionClear;
            aspect.storeAction = MTLStoreActionStore;
            clear.depthAttachment.clearDepth = 0.25;
            clear.stencilAttachment.clearStencil = 0x5a;
            id<MTLCommandBuffer> c = [q commandBuffer];
            [[c renderCommandEncoderWithDescriptor:clear] endEncoding];
            [c commit];
            [c waitUntilCompleted];
            id<MTLCommandBuffer> r = [q commandBuffer];
            id<MTLBlitCommandEncoder> e = [r blitCommandEncoder];
            [e copyFromTexture:t
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:depth
                       destinationOffset:0
                  destinationBytesPerRow:w * t->_raw.bpp
                destinationBytesPerImage:w * h * t->_raw.bpp];
            [e endEncoding];
            [r commit];
            [r waitUntilCompleted];
            char what[128];
            snprintf(what, sizeof what,
                     "format %lu, a render target also sampled: its own texture header, cleared and read back",
                     (unsigned long)f);
            expect(t->_rawOk && t->_raw.zeta && t->_raw.tic && [path(c) isEqualToString:@"raw"] &&
                       [path(r) isEqualToString:@"raw"],
                   what, [NSString stringWithFormat:@"%@ / %@", path(c), path(r)]);
        }
        printf("raw facade: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
