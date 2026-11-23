// SPDX-License-Identifier: MIT
// What this Mac's Apple GPU does, API validation off, with the render passes TegraGM20BMetalPassErrors.mm names: each case runs in
// a process of its own (a crash names itself) and prints the command buffer's status and what the textures hold after it.
// usage: passoracle <case> [depth] [stencil reference]   (passoracle list: the cases)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

NSString *const kSource =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; uint layer [[render_target_array_index]]; };\n"
    @"vertex V cover(uint v [[vertex_id]], uint i [[instance_id]], constant float &z [[buffer(0)]]) { V o;\n"
    @"  o.p = float4((v & 1) ? 1.0 : -1.0, (v & 2) ? 1.0 : -1.0, z, 1); o.layer = i; return o; }\n"
    @"vertex V half_cover(uint v [[vertex_id]], constant float &z [[buffer(0)]]) { V o;\n"
    @"  o.p = float4((v & 1) ? 0.0 : -1.0, (v & 2) ? 1.0 : -1.0, z, 1); o.layer = 0; return o; }\n"
    @"fragment float4 white(V in [[stage_in]]) { return float4(1); }\n"
    @"fragment float4 layer_colour(V in [[stage_in]]) { return float4(float(in.layer + 1) / 8.0, 0.25, 0.5, 1); }\n"
    @"struct D { float depth [[depth(any)]]; };\n"
    @"fragment D depth_of(V in [[stage_in]], constant float &d [[buffer(0)]]) { D o; o.depth = d; return o; }\n";

id<MTLDevice> gDevice;
float gZ = 0.5f;
uint32_t gRef = 3;
id<MTLLibrary> gLibrary;
id<MTLCommandQueue> gQueue;

id<MTLTexture> make(MTLPixelFormat f, MTLTextureType type = MTLTextureType2D, NSUInteger samples = 1,
                    MTLStorageMode storage = MTLStorageModePrivate, NSUInteger layers = 1, NSUInteger levels = 1,
                    NSUInteger w = 16, NSUInteger h = 16, NSUInteger depth = 1)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = f;
    d.textureType = type;
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.sampleCount = samples;
    d.arrayLength = layers;
    d.mipmapLevelCount = levels;
    d.storageMode = storage;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [gDevice newTextureWithDescriptor:d];
}

id<MTLRenderPipelineState> state(MTLPixelFormat colour, const char *vertex, const char *fragment,
                                 MTLPixelFormat depth = MTLPixelFormatInvalid,
                                 MTLPixelFormat stencil = MTLPixelFormatInvalid, NSUInteger samples = 1,
                                 bool layered = false)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLibrary newFunctionWithName:@(vertex)];
    d.fragmentFunction = [gLibrary newFunctionWithName:@(fragment)];
    d.colorAttachments[0].pixelFormat = colour;
    d.depthAttachmentPixelFormat = depth;
    d.stencilAttachmentPixelFormat = stencil;
    d.rasterSampleCount = samples;
    (void)layered;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    NSError *e = nil;
    id<MTLRenderPipelineState> s = [gDevice newRenderPipelineStateWithDescriptor:d error:&e];
    if (!s)
        printf("state %s/%s: %s\n", vertex, fragment, e.description.UTF8String);
    return s;
}

void finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit];
    [cb waitUntilCompleted];
    printf("%s: status %ld error %ld %s\n", what, (long)cb.status, (long)cb.error.code,
           cb.error ? cb.error.localizedDescription.UTF8String : "");
}

// The bytes of `t`'s level `level`, slice `slice` through a shared texture blit (rows packed)
std::vector<uint8_t> bytes(id<MTLTexture> t, NSUInteger bpp, NSUInteger level = 0, NSUInteger slice = 0)
{
    const NSUInteger w = std::max<NSUInteger>(t.width >> level, 1), h = std::max<NSUInteger>(t.height >> level, 1);
    id<MTLBuffer> b = [gDevice newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t
                     sourceSlice:slice
                     sourceLevel:level
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:b
               destinationOffset:0
          destinationBytesPerRow:w * bpp
        destinationBytesPerImage:w * h * bpp];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *p = static_cast<const uint8_t *>(b.contents);
    return {p, p + w * h * bpp};
}

// The distinct 4-byte texels of `b` and their counts
void histogram(const char *what, const std::vector<uint8_t> &b, NSUInteger bpp = 4)
{
    std::map<std::string, int> seen;
    for (size_t i = 0; i + bpp <= b.size(); i += bpp) {
        char k[64] = "";
        for (NSUInteger c = 0; c < bpp; c++)
            snprintf(k + strlen(k), sizeof k - strlen(k), "%02x", b[i + c]);
        seen[k]++;
    }
    printf("%s:", what);
    int shown = 0;
    for (const auto &[k, n] : seen) {
        if (shown++ == 8) {
            printf(" ... (%zu distinct)", seen.size());
            break;
        }
        printf(" %s x%d", k.c_str(), n);
    }
    printf("\n");
}

MTLRenderPassDescriptor *colourPass(id<MTLTexture> t, MTLLoadAction load = MTLLoadActionClear)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    return p;
}

id<MTLDepthStencilState> depthTest(MTLCompareFunction f, bool write)
{
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = f;
    d.depthWriteEnabled = write;
    return [gDevice newDepthStencilStateWithDescriptor:d];
}

id<MTLDepthStencilState> stencilTest(MTLCompareFunction f, MTLStencilOperation pass)
{
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    MTLStencilDescriptor *s = [MTLStencilDescriptor new];
    s.stencilCompareFunction = f;
    s.depthStencilPassOperation = pass;
    d.frontFaceStencil = s;
    d.backFaceStencil = s;
    return [gDevice newDepthStencilStateWithDescriptor:d];
}

void drawQuad(id<MTLRenderCommandEncoder> e, float z, NSUInteger instances = 1, bool half = false)
{
    [e setVertexBytes:&z length:4 atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:instances];
    (void)half;
}

// A memoryless depth loaded: earlier in the command buffer a pass cleared it to 0.25 and "stored" it (Store, which Metal's
// validation stops on memoryless); the next pass loads it (clearDepth 0.75 named but not used) and draws white at z 0.5
// under LessEqual: white where the loaded depth >= 0.5
void memorylessDepthLoaded(bool stored)
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm),
                   z = make(MTLPixelFormatDepth32Float, MTLTextureType2D, 1, MTLStorageModeMemoryless);
    id<MTLRenderPipelineState> s = state(MTLPixelFormatRGBA8Unorm, "cover", "white", MTLPixelFormatDepth32Float);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *p = colourPass(colour, pass ? MTLLoadActionLoad : MTLLoadActionClear);
        p.depthAttachment.texture = z;
        p.depthAttachment.loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear;
        p.depthAttachment.storeAction = pass == 0 && stored ? MTLStoreActionStore : MTLStoreActionDontCare;
        p.depthAttachment.clearDepth = pass ? 0.75 : 0.25;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        if (pass) {
            [e setRenderPipelineState:s];
            [e setDepthStencilState:depthTest(MTLCompareFunctionLessEqual, false)];
            drawQuad(e, gZ);
        }
        [e endEncoding];
    }
    finish(cb, stored ? "memoryless depth stored then loaded" : "memoryless depth loaded");
    histogram("colour", bytes(colour, 4));
}

// The same of a memoryless stencil: cleared to 3, then loaded (clearStencil 9 named) and drawn under Equal 3
void memorylessStencilLoaded()
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm),
                   s8 = make(MTLPixelFormatStencil8, MTLTextureType2D, 1, MTLStorageModeMemoryless);
    id<MTLRenderPipelineState> s =
        state(MTLPixelFormatRGBA8Unorm, "cover", "white", MTLPixelFormatInvalid, MTLPixelFormatStencil8);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *p = colourPass(colour, pass ? MTLLoadActionLoad : MTLLoadActionClear);
        p.stencilAttachment.texture = s8;
        p.stencilAttachment.loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear;
        p.stencilAttachment.storeAction = pass ? MTLStoreActionDontCare : MTLStoreActionStore;
        p.stencilAttachment.clearStencil = pass ? 9 : 3;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        if (pass) {
            [e setRenderPipelineState:s];
            [e setDepthStencilState:stencilTest(MTLCompareFunctionEqual, MTLStencilOperationKeep)];
            [e setStencilReferenceValue:gRef];
            drawQuad(e, 0.5);
        }
        [e endEncoding];
    }
    finish(cb, "memoryless stencil stored then loaded, drawn under Equal 3");
    histogram("colour", bytes(colour, 4));
}

void colourOf(MTLPixelFormat f, NSUInteger bpp, const char *what)
{
    id<MTLTexture> t = make(f);
    if (!t) {
        printf("%s: no texture\n", what);
        return;
    }
    id<MTLCommandBuffer> fill = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> b = [fill blitCommandEncoder];
    [b fillBuffer:[gDevice newBufferWithLength:16 options:0] range:NSMakeRange(0, 16) value:0];
    [b endEncoding];
    [fill commit];
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, what);
    if (bpp)
        histogram("texture", bytes(t, bpp), bpp);
}

void levelPast()
{
    id<MTLTexture> t = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2D, 1, MTLStorageModePrivate, 1, 2);
    id<MTLCommandBuffer> pre = [gQueue commandBuffer];
    [[pre renderCommandEncoderWithDescriptor:colourPass(t)] endEncoding];
    MTLRenderPassDescriptor *one = colourPass(t);
    one.colorAttachments[0].level = 1;
    [[pre renderCommandEncoderWithDescriptor:one] endEncoding];
    [pre commit];
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].level = 3;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "level 3 of a 2-level texture");
    histogram("level 0", bytes(t, 4, 0));
    histogram("level 1", bytes(t, 4, 1));
}

void slicePast()
{
    id<MTLTexture> t = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2DArray, 1, MTLStorageModePrivate, 2);
    id<MTLCommandBuffer> pre = [gQueue commandBuffer];
    for (NSUInteger s = 0; s < 2; s++) {
        MTLRenderPassDescriptor *p = colourPass(t);
        p.colorAttachments[0].slice = s;
        [[pre renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    [pre commit];
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].slice = 5;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "slice 5 of a 2-layer array");
    histogram("slice 0", bytes(t, 4, 0, 0));
    histogram("slice 1", bytes(t, 4, 0, 1));
}

void slicePastOfPlain()
{
    id<MTLTexture> t = make(MTLPixelFormatRGBA8Unorm);
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].slice = 1;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "slice 1 of a 2D texture");
    histogram("texture", bytes(t, 4));
}

void layersPast()
{
    id<MTLTexture> t = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2DArray, 1, MTLStorageModePrivate, 2);
    id<MTLRenderPipelineState> s =
        state(MTLPixelFormatRGBA8Unorm, "cover", "layer_colour", MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, true);
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].slice = 1;
    p.renderTargetArrayLength = 2;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:s];
    drawQuad(e, 0.5, 2);
    [e endEncoding];
    finish(cb, "renderTargetArrayLength 2 from slice 1 of 2 layers");
    histogram("slice 0", bytes(t, 4, 0, 0));
    histogram("slice 1", bytes(t, 4, 0, 1));
}

void samplesMixed()
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm),
                   z = make(MTLPixelFormatDepth32Float, MTLTextureType2DMultisample, 4);
    id<MTLRenderPipelineState> s = state(MTLPixelFormatRGBA8Unorm, "cover", "white", MTLPixelFormatDepth32Float);
    MTLRenderPassDescriptor *p = colourPass(colour);
    p.depthAttachment.texture = z;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.clearDepth = 1;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:s];
    [e setDepthStencilState:depthTest(MTLCompareFunctionLess, true)];
    drawQuad(e, 0.5);
    [e endEncoding];
    finish(cb, "a one-sample colour beside a 4-sample depth");
    histogram("colour", bytes(colour, 4));
}

void resolveOneSample()
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm), into = make(MTLPixelFormatRGBA8Unorm);
    id<MTLCommandBuffer> pre = [gQueue commandBuffer];
    MTLRenderPassDescriptor *c = colourPass(into);
    c.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    [[pre renderCommandEncoderWithDescriptor:c] endEncoding];
    [pre commit];
    MTLRenderPassDescriptor *p = colourPass(colour);
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    p.colorAttachments[0].resolveTexture = into;
    p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "a one-sample colour resolved (StoreAndMultisampleResolve)");
    histogram("colour", bytes(colour, 4));
    histogram("resolve texture", bytes(into, 4));
}

void resolveOtherCount()
{
    id<MTLTexture> four = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2DMultisample, 4),
                   two = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2DMultisample, 2),
                   read = make(MTLPixelFormatRGBA8Unorm);
    id<MTLCommandBuffer> pre = [gQueue commandBuffer];
    MTLRenderPassDescriptor *c = colourPass(two);
    c.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    [[pre renderCommandEncoderWithDescriptor:c] endEncoding];
    [pre commit];
    MTLRenderPassDescriptor *p = colourPass(four);
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    p.colorAttachments[0].resolveTexture = two;
    p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "a 4-sample colour resolved into a 2-sample texture");
    MTLRenderPassDescriptor *r = colourPass(two);
    r.colorAttachments[0].loadAction = MTLLoadActionLoad;
    r.colorAttachments[0].resolveTexture = read;
    r.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    id<MTLCommandBuffer> back = [gQueue commandBuffer];
    [[back renderCommandEncoderWithDescriptor:r] endEncoding];
    finish(back, "the 2-sample texture resolved for reading");
    histogram("2-sample texture (green before)", bytes(read, 4));
}

void resolveOneSampleDontCare()
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm), into = make(MTLPixelFormatRGBA8Unorm);
    MTLRenderPassDescriptor *p = colourPass(colour);
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    p.colorAttachments[0].resolveTexture = into;
    p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb, "a one-sample colour resolved (MultisampleResolve)");
    histogram("resolve texture", bytes(into, 4));
}

void depthOneSampleResolved()
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm), z = make(MTLPixelFormatDepth32Float),
                   into = make(MTLPixelFormatDepth32Float, MTLTextureType2D, 1, MTLStorageModeShared);
    id<MTLRenderPipelineState> s = state(MTLPixelFormatRGBA8Unorm, "cover", "white", MTLPixelFormatDepth32Float);
    MTLRenderPassDescriptor *p = colourPass(colour);
    p.depthAttachment.texture = z;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.clearDepth = 0.75;
    p.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
    p.depthAttachment.resolveTexture = into;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:s];
    [e setDepthStencilState:depthTest(MTLCompareFunctionAlways, true)];
    drawQuad(e, 0.25);
    [e endEncoding];
    finish(cb, "a one-sample depth resolved");
    histogram("colour", bytes(colour, 4));
    histogram("resolve depth", bytes(into, 4));
}

void wrongAspect(bool depthOfColour)
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm);
    id<MTLTexture> z = depthOfColour ? make(MTLPixelFormatRGBA8Unorm) : make(MTLPixelFormatStencil8);
    MTLRenderPassDescriptor *p = colourPass(colour);
    p.depthAttachment.texture = z;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.depthAttachment.clearDepth = 0.5;
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    finish(cb,
           depthOfColour ? "an RGBA8 texture as the depth attachment" : "a Stencil8 texture as the depth attachment");
    histogram("colour", bytes(colour, 4));
    if (depthOfColour)
        histogram("the RGBA8 'depth'", bytes(z, 4));
}

// A memoryless 4-sample colour loaded (an earlier pass cleared it green and "stored" it), resolved into a plain texture
void memorylessColourLoaded()
{
    id<MTLTexture> m = make(MTLPixelFormatRGBA8Unorm, MTLTextureType2DMultisample, 4, MTLStorageModeMemoryless),
                   into = make(MTLPixelFormatRGBA8Unorm);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = m;
        p.colorAttachments[0].loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear;
        p.colorAttachments[0].clearColor = pass ? MTLClearColorMake(0, 0, 1, 1) : MTLClearColorMake(0, 1, 0, 1);
        p.colorAttachments[0].storeAction = pass ? MTLStoreActionMultisampleResolve : MTLStoreActionStore;
        if (pass)
            p.colorAttachments[0].resolveTexture = into;
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    }
    finish(cb, "a memoryless 4-sample colour loaded, resolved");
    histogram("resolved", bytes(into, 4));
}

// A block texture's bytes before and after a pass that clears it as a colour attachment, and draws into it
void blocksCleared(MTLPixelFormat f, NSUInteger blockBytes, bool draw)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [gDevice newTextureWithDescriptor:d];
    const NSUInteger row = 4 * blockBytes;
    std::vector<uint8_t> in(row * 4);
    for (size_t i = 0; i < in.size(); i++)
        in[i] = uint8_t(i * 37 + 11);
    [t replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:in.data() bytesPerRow:row];
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    if (draw) {
        [e setRenderPipelineState:state(MTLPixelFormatRGBA8Unorm, "cover", "white")];
        drawQuad(e, 0.5);
    }
    [e endEncoding];
    finish(cb, draw ? "a block texture drawn into as a colour attachment"
                    : "a block texture cleared as a colour attachment");
    std::vector<uint8_t> out(in.size());
    [t getBytes:out.data() bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    size_t changed = 0;
    for (size_t i = 0; i < in.size(); i++)
        changed += in[i] != out[i];
    printf("bytes changed %zu of %zu; first block after:", changed, in.size());
    for (NSUInteger i = 0; i < blockBytes; i++)
        printf(" %02x", out[i]);
    printf("\n");
}

// A Depth32Float texture as the colour attachment, drawn (red 0.5) over its clear (0.25)
void depthAsColourDrawn()
{
    id<MTLTexture> t = make(MTLPixelFormatDepth32Float);
    MTLRenderPassDescriptor *p = colourPass(t);
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0, 0, 1);
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    id<MTLRenderPipelineState> s = state(MTLPixelFormatR32Float, "half_cover", "white");
    if (s) {
        [e setRenderPipelineState:s];
        drawQuad(e, 0.5);
    }
    [e endEncoding];
    finish(cb, "a Depth32Float colour attachment drawn half white over a 0.25 clear (R32Float state)");
    histogram("texture", bytes(t, 4));
}

// A Depth32Float_Stencil8 texture: pass 1 clears depth 0.25 and stencil 7 and stores both; pass 2 attaches one aspect alone
// (`depthAlone`: the depth, cleared 0.75 and stored; else the stencil, cleared 9 and stored); pass 3 loads both and draws
// white at z 0.5 under depth LessEqual and stencil Equal `gRef`: what the aspect pass 2 left unattached holds
void oneAspect(bool depthAlone)
{
    id<MTLTexture> colour = make(MTLPixelFormatRGBA8Unorm), z = make(MTLPixelFormatDepth32Float_Stencil8);
    id<MTLRenderPipelineState> s = state(MTLPixelFormatRGBA8Unorm, "cover", "white",
                                         MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatDepth32Float_Stencil8);
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLessEqual;
    MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
    sd.stencilCompareFunction = MTLCompareFunctionEqual;
    dd.frontFaceStencil = sd;
    dd.backFaceStencil = sd;
    id<MTLDepthStencilState> test = [gDevice newDepthStencilStateWithDescriptor:dd];
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    for (int pass = 0; pass < 3; pass++) {
        MTLRenderPassDescriptor *p = colourPass(colour, pass == 2 ? MTLLoadActionClear : MTLLoadActionDontCare);
        const bool depth = pass != 1 || depthAlone, stencil = pass != 1 || !depthAlone;
        if (depth) {
            p.depthAttachment.texture = z;
            p.depthAttachment.loadAction = pass == 2 ? MTLLoadActionLoad : MTLLoadActionClear;
            p.depthAttachment.storeAction = MTLStoreActionStore;
            p.depthAttachment.clearDepth = pass ? 0.75 : 0.25;
        }
        if (stencil) {
            p.stencilAttachment.texture = z;
            p.stencilAttachment.loadAction = pass == 2 ? MTLLoadActionLoad : MTLLoadActionClear;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
            p.stencilAttachment.clearStencil = pass ? 9 : 7;
        }
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        if (pass == 2) {
            [e setRenderPipelineState:s];
            [e setDepthStencilState:test];
            [e setStencilReferenceValue:gRef];
            drawQuad(e, gZ);
        }
        [e endEncoding];
    }
    finish(cb, depthAlone ? "depth attached alone in pass 2" : "stencil attached alone in pass 2");
    histogram("colour", bytes(colour, 4));
}

}  // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    @autoreleasepool {
        gDevice = MTLCreateSystemDefaultDevice();
        gQueue = [gDevice newCommandQueue];
        NSError *error = nil;
        gLibrary = [gDevice newLibraryWithSource:kSource options:nil error:&error];
        if (!gLibrary) {
            printf("library: %s\n", error.description.UTF8String);
            return 1;
        }
        const std::string c = argc > 1 ? argv[1] : "list";
        if (argc > 2)
            gZ = float(atof(argv[2])), gRef = uint32_t(atoi(argv[2]));
        if (argc > 3)
            gRef = uint32_t(atoi(argv[3]));
        const std::map<std::string, void (*)()> cases = {
            {"memoryless_depth_loaded",
             [] {
                 memorylessDepthLoaded(false);
             }},
            {"memoryless_depth_stored_loaded",
             [] {
                 memorylessDepthLoaded(true);
             }},
            {"memoryless_stencil",
             [] {
                 memorylessStencilLoaded();
             }},
            {"memoryless_colour", memorylessColourLoaded},
            {"depth_alone",
             [] {
                 oneAspect(true);
             }},
            {"stencil_alone",
             [] {
                 oneAspect(false);
             }},
            {"astc_bytes",
             [] {
                 blocksCleared(MTLPixelFormatASTC_4x4_LDR, 16, false);
             }},
            {"astc_drawn",
             [] {
                 blocksCleared(MTLPixelFormatASTC_4x4_LDR, 16, true);
             }},
            {"bc1_bytes",
             [] {
                 blocksCleared(MTLPixelFormatBC1_RGBA, 8, false);
             }},
            {"depth_as_colour_drawn", depthAsColourDrawn},
            {"astc_colour",
             [] {
                 colourOf(MTLPixelFormatASTC_4x4_LDR, 0, "an ASTC 4x4 colour attachment cleared");
             }},
            {"bc1_colour",
             [] {
                 colourOf(MTLPixelFormatBC1_RGBA, 0, "a BC1 colour attachment cleared");
             }},
            {"depth_colour",
             [] {
                 colourOf(MTLPixelFormatDepth32Float, 4, "a Depth32Float colour attachment cleared");
             }},
            {"level_past", levelPast},
            {"slice_past", slicePast},
            {"slice_of_plain", slicePastOfPlain},
            {"layers_past", layersPast},
            {"samples_mixed", samplesMixed},
            {"resolve_one_sample", resolveOneSample},
            {"resolve_one_sample_dontcare", resolveOneSampleDontCare},
            {"resolve_other_count", resolveOtherCount},
            {"depth_one_sample_resolved", depthOneSampleResolved},
            {"rgba8_as_depth",
             [] {
                 wrongAspect(true);
             }},
            {"stencil8_as_depth",
             [] {
                 wrongAspect(false);
             }},
        };
        if (c == "list") {
            for (const auto &[name, f] : cases)
                printf("%s\n", name.c_str());
            return 0;
        }
        const auto it = cases.find(c);
        if (it == cases.end())
            return 2;
        it->second();
    }
    return 0;
}
