// SPDX-License-Identifier: MIT
// passformsoracle.mm's zeta cases: depth/stencil attachments that differ in texture, level, slice, size or samples,
// and depth/stencil resolves into a level, a slice, a memoryless texture or another format
#include "passformsoracle.h"

namespace passforms {

constexpr MTLPixelFormat kD32 = MTLPixelFormatDepth32Float, kD32S8 = MTLPixelFormatDepth32Float_Stencil8,
                         kS8 = MTLPixelFormatStencil8, kX32S8 = MTLPixelFormatX32_Stencil8;

void depths(const char *what, const std::vector<uint8_t> &b)
{
    std::map<float, int> seen;
    for (size_t i = 0; i + 4 <= b.size(); i += 4) {
        float f;
        memcpy(&f, &b[i], 4);
        seen[f]++;
    }
    printf("  %s depth:", what);
    for (const auto &[f, n] : seen)
        printf(" %g x%d", f, n);
    printf("\n");
}

// Depth (as float) and stencil of `t` at `level`, `slice`, whatever its format
void zetaOf(const char *what, id<MTLTexture> t, NSUInteger level = 0, NSUInteger slice = 0)
{
    char name[96];
    snprintf(name, sizeof name, "%s level %lu slice %lu", what, (unsigned long)level, (unsigned long)slice);
    if (t.pixelFormat == kD32S8) {
        depths(name, bytes(t, 4, level, slice, MTLBlitOptionDepthFromDepthStencil));
        histogram((std::string(name) + " stencil").c_str(),
                  bytes(t, 1, level, slice, MTLBlitOptionStencilFromDepthStencil), 1);
    } else if (t.pixelFormat == kS8) {
        histogram((std::string(name) + " stencil").c_str(), bytes(t, 1, level, slice), 1);
    } else {
        depths(name, bytes(t, 4, level, slice));
    }
}

id<MTLDepthStencilState> writesBoth()
{
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionAlways;
    d.depthWriteEnabled = YES;
    MTLStencilDescriptor *s = [MTLStencilDescriptor new];
    s.stencilCompareFunction = MTLCompareFunctionAlways;
    s.depthStencilPassOperation = MTLStencilOperationReplace;
    d.frontFaceStencil = s;
    d.backFaceStencil = s;
    return [gDevice newDepthStencilStateWithDescriptor:d];
}

// Clears level `level`, slice `slice` of zeta `t` to `depth` and `stencil` (each aspect it has)
void clearZeta(id<MTLTexture> t, NSUInteger level, NSUInteger slice, double depth, uint32_t stencil)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool hasDepth = t.pixelFormat != kS8, hasStencil = t.pixelFormat == kS8 || t.pixelFormat == kD32S8;
    if (hasDepth) {
        p.depthAttachment.texture = t;
        p.depthAttachment.level = level;
        p.depthAttachment.slice = slice;
        p.depthAttachment.loadAction = MTLLoadActionClear;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        p.depthAttachment.clearDepth = depth;
    }
    if (hasStencil) {
        p.stencilAttachment.texture = t;
        p.stencilAttachment.level = level;
        p.stencilAttachment.slice = slice;
        p.stencilAttachment.loadAction = MTLLoadActionClear;
        p.stencilAttachment.storeAction = MTLStoreActionStore;
        p.stencilAttachment.clearStencil = stencil;
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
}

struct Aspect {
    id<MTLTexture> t;
    NSUInteger level = 0, slice = 0;
    MTLLoadAction load = MTLLoadActionLoad;
    MTLStoreAction store = MTLStoreActionStore;
};

// A pass of depth `d` and stencil `s` (either may be nil), colour `colour` beside them, drawing depth 0.5 and stencil 7
// (or, with `drawn` false, nothing: the clears alone, depth 0.75 and stencil 9)
void zetaPass(const char *what, Aspect d, Aspect s, bool drawn, id<MTLTexture> colour = nil, NSUInteger samples = 1)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    if (d.t) {
        p.depthAttachment.texture = d.t;
        p.depthAttachment.level = d.level;
        p.depthAttachment.slice = d.slice;
        p.depthAttachment.loadAction = d.load;
        p.depthAttachment.storeAction = d.store;
        p.depthAttachment.clearDepth = 0.75;
    }
    if (s.t) {
        p.stencilAttachment.texture = s.t;
        p.stencilAttachment.level = s.level;
        p.stencilAttachment.slice = s.slice;
        p.stencilAttachment.loadAction = s.load;
        p.stencilAttachment.storeAction = s.store;
        p.stencilAttachment.clearStencil = 9;
    }
    if (colour) {
        p.colorAttachments[0].texture = colour;
        p.colorAttachments[0].loadAction = MTLLoadActionClear;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    if (drawn) {
        id<MTLRenderPipelineState> st =
            state(colour ? colour.pixelFormat : MTLPixelFormatInvalid, colour ? "colour_of" : "depth_of",
                  d.t ? d.t.pixelFormat == kX32S8 ? kD32S8 : d.t.pixelFormat : MTLPixelFormatInvalid,
                  s.t ? s.t.pixelFormat == kX32S8 ? kD32S8 : s.t.pixelFormat : MTLPixelFormatInvalid, samples);
        if (st) {
            [e setRenderPipelineState:st];
            [e setDepthStencilState:writesBoth()];
            [e setStencilReferenceValue:7];
            const float z = 0.5f;
            const float white[4] = {1, 1, 1, 1};
            [e setVertexBytes:&z length:4 atIndex:0];
            if (colour)
                [e setFragmentBytes:white length:16 atIndex:0];
            else
                [e setFragmentBytes:&z length:4 atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
    [e endEncoding];
    finish(cb, what);
}

// Two Depth32Float_Stencil8 textures, one the depth attachment and the other the stencil one
void zetaTwoTextures(bool drawn)
{
    id<MTLTexture> a = make(kD32S8), b = make(kD32S8);
    clearZeta(a, 0, 0, 0.1, 1);
    clearZeta(b, 0, 0, 0.2, 2);
    Aspect d{a}, s{b};
    if (!drawn)
        d.load = s.load = MTLLoadActionClear;
    zetaPass(drawn ? "depth A, stencil B, drawn 0.5/7" : "depth A, stencil B, cleared 0.75/9", d, s, drawn);
    zetaOf("A (cleared .1/1)", a);
    zetaOf("B (cleared .2/2)", b);
}

void zetaTwoLevels(bool drawn)
{
    id<MTLTexture> a = make(kD32S8, 64, 64, MTLTextureType2D, 1, 1, 2);
    clearZeta(a, 0, 0, 0.1, 1);
    clearZeta(a, 1, 0, 0.2, 2);
    Aspect d{a, 0}, s{a, 1};
    if (!drawn)
        d.load = s.load = MTLLoadActionClear;
    zetaPass(drawn ? "depth level 0, stencil level 1, drawn" : "depth level 0, stencil level 1, cleared", d, s, drawn);
    zetaOf("level 0 (.1/1)", a, 0);
    zetaOf("level 1 (.2/2)", a, 1);
}

void zetaTwoSlices(bool drawn)
{
    id<MTLTexture> a = make(kD32S8, 32, 32, MTLTextureType2DArray, 1, 2);
    clearZeta(a, 0, 0, 0.1, 1);
    clearZeta(a, 0, 1, 0.2, 2);
    Aspect d{a, 0, 0}, s{a, 0, 1};
    if (!drawn)
        d.load = s.load = MTLLoadActionClear;
    zetaPass(drawn ? "depth slice 0, stencil slice 1, drawn" : "depth slice 0, stencil slice 1, cleared", d, s, drawn);
    zetaOf("slice 0 (.1/1)", a, 0, 0);
    zetaOf("slice 1 (.2/2)", a, 0, 1);
}

// A Depth32Float and a Stencil8 of different sizes, a colour of the depth's size beside them, drawn white
void pairSizes()
{
    id<MTLTexture> d = make(kD32, 64, 64), s = make(kS8, 32, 32), c = make(MTLPixelFormatRGBA8Unorm, 64, 64);
    clearZeta(d, 0, 0, 0.1, 0);
    clearZeta(s, 0, 0, 0, 2);
    zetaPass("Depth32Float 64x64 + Stencil8 32x32 + RGBA8 64x64, drawn", {d}, {s}, true, c);
    zetaOf("depth (.1)", d);
    zetaOf("stencil (2)", s);
    histogram("colour (cleared 0, drawn ff)", bytes(c, 4), 4);
    id<MTLTexture> d2 = make(kD32, 32, 32), s2 = make(kS8, 64, 64), c2 = make(MTLPixelFormatRGBA8Unorm, 64, 64);
    clearZeta(d2, 0, 0, 0.1, 0);
    clearZeta(s2, 0, 0, 0, 2);
    zetaPass("Depth32Float 32x32 + Stencil8 64x64 + RGBA8 64x64, drawn", {d2}, {s2}, true, c2);
    zetaOf("depth (.1)", d2);
    zetaOf("stencil (2)", s2);
    histogram("colour (cleared 0, drawn ff)", bytes(c2, 4), 4);
}

std::vector<float> msDepth(id<MTLTexture> t)
{
    const NSUInteger n = t.width * t.height * t.sampleCount;
    id<MTLBuffer> b = [gDevice newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    NSError *e = nil;
    id<MTLComputePipelineState> k =
        [gDevice newComputePipelineStateWithFunction:[gLibrary newFunctionWithName:@"read_ms_depth"] error:&e];
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k];
    [c setTexture:t atIndex:0];
    [c setBuffer:b offset:0 atIndex:0];
    [c dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [c endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *f = static_cast<const float *>(b.contents);
    return {f, f + n};
}

// A 4x Depth32Float beside a one-sample Stencil8, drawn by a 4x state then a 1x one
void pairSamples()
{
    for (NSUInteger samples : {4u, 1u}) {
        id<MTLTexture> d = make(kD32, 32, 32, MTLTextureType2DMultisample, 4), s = make(kS8, 32, 32);
        Aspect da{d}, sa{s};
        da.load = sa.load = MTLLoadActionClear;
        char what[96];
        snprintf(what, sizeof what, "Depth32Float 4x + Stencil8 1x, drawn by a %lux state", (unsigned long)samples);
        zetaPass(what, da, sa, true, nil, samples);
        std::map<float, int> seen;
        for (float f : msDepth(d))
            seen[f]++;
        printf("  depth samples:");
        for (const auto &[f, n] : seen)
            printf(" %g x%d", f, n);
        printf("\n");
        zetaOf("stencil (cleared 9)", s);
    }
}

// A Depth32Float_Stencil8 as depth beside a Stencil8 as stencil, and a Depth32Float beside a Depth32Float_Stencil8 as stencil
void mixedPairs()
{
    {
        id<MTLTexture> a = make(kD32S8), b = make(kS8);
        clearZeta(a, 0, 0, 0.1, 1);
        clearZeta(b, 0, 0, 0, 2);
        Aspect da{a}, sb{b};
        da.load = sb.load = MTLLoadActionClear;
        zetaPass("depth Depth32Float_Stencil8 A, stencil Stencil8 B, cleared .75/9", da, sb, false);
        zetaOf("A (.1/1)", a);
        zetaOf("B (2)", b);
    }
    {
        id<MTLTexture> a = make(kD32), b = make(kD32S8);
        clearZeta(a, 0, 0, 0.1, 0);
        clearZeta(b, 0, 0, 0.2, 2);
        Aspect da{a}, sb{b};
        da.load = sb.load = MTLLoadActionClear;
        zetaPass("depth Depth32Float A, stencil Depth32Float_Stencil8 B, cleared .75/9", da, sb, false);
        zetaOf("A (.1)", a);
        zetaOf("B (.2/2)", b);
    }
}

// The X32_Stencil8 view of a Depth32Float_Stencil8 as the stencil attachment: beside the texture as depth, and alone
void stencilView()
{
    id<MTLTexture> a = make(kD32S8, 32, 32, MTLTextureType2D, 1, 1, 1, MTLStorageModePrivate,
                            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView);
    clearZeta(a, 0, 0, 0.1, 1);
    id<MTLTexture> v = [a newTextureViewWithPixelFormat:kX32S8];
    printf("view: %s\n", v ? "made" : "nil");
    if (!v)
        return;
    zetaPass("depth A, stencil A's X32_Stencil8 view, drawn", {a}, {v}, true);
    zetaOf("A (.1/1)", a);
    clearZeta(a, 0, 0, 0.1, 1);
    zetaPass("stencil A's X32_Stencil8 view alone, drawn", {}, {v}, true);
    zetaOf("A (.1/1)", a);
}

// A 4x zeta resolved by filter `filter` (or stencil sample 0) into `into` at `level`, `slice`; depth per sample .2 + .1 * s
void resolveZeta(const char *what, MTLPixelFormat from, id<MTLTexture> into, NSUInteger level, NSUInteger slice,
                 MTLMultisampleDepthResolveFilter filter, bool stencil)
{
    id<MTLTexture> ms = make(from, 32, 32, MTLTextureType2DMultisample, 4);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassAttachmentDescriptor *a = stencil ? (MTLRenderPassAttachmentDescriptor *)p.stencilAttachment
                                                   : (MTLRenderPassAttachmentDescriptor *)p.depthAttachment;
    a.texture = ms;
    a.loadAction = MTLLoadActionClear;
    a.storeAction = MTLStoreActionMultisampleResolve;
    a.resolveTexture = into;
    a.resolveLevel = level;
    a.resolveSlice = slice;
    p.depthAttachment.depthResolveFilter = filter;
    p.stencilAttachment.clearStencil = 9;
    p.depthAttachment.clearDepth = 0.75;
    if (from == kD32S8) {
        MTLRenderPassAttachmentDescriptor *o = stencil ? (MTLRenderPassAttachmentDescriptor *)p.depthAttachment
                                                       : (MTLRenderPassAttachmentDescriptor *)p.stencilAttachment;
        o.texture = ms;
        o.loadAction = MTLLoadActionClear;
        o.storeAction = MTLStoreActionDontCare;
    }
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    const bool depthAspect = from != kS8;
    id<MTLRenderPipelineState> st =
        state(MTLPixelFormatInvalid, "depth_by_sample", depthAspect ? from : MTLPixelFormatInvalid,
              from == kS8 || from == kD32S8 ? from : MTLPixelFormatInvalid, 4);
    if (st) {
        [e setRenderPipelineState:st];
        [e setDepthStencilState:writesBoth()];
        [e setStencilReferenceValue:7];
        const float z = 0.5f;
        [e setVertexBytes:&z length:4 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [e endEncoding];
    finish(cb, what);
}

void zetaResolves()
{
    {
        id<MTLTexture> into = make(kD32, 64, 64, MTLTextureType2D, 1, 1, 2);
        clearZeta(into, 0, 0, 0.9, 0);
        clearZeta(into, 1, 0, 0.9, 0);
        resolveZeta("4x Depth32Float max into level 1 of a 64x64", kD32, into, 1, 0,
                    MTLMultisampleDepthResolveFilterMax, false);
        zetaOf("into (.9)", into, 0);
        zetaOf("into (.9)", into, 1);
    }
    {
        id<MTLTexture> into = make(kD32, 32, 32, MTLTextureType2DArray, 1, 2);
        clearZeta(into, 0, 0, 0.9, 0);
        clearZeta(into, 0, 1, 0.9, 0);
        resolveZeta("4x Depth32Float max into slice 1", kD32, into, 0, 1, MTLMultisampleDepthResolveFilterMax, false);
        zetaOf("into (.9)", into, 0, 0);
        zetaOf("into (.9)", into, 0, 1);
    }
    {
        id<MTLTexture> into = make(kS8, 64, 64, MTLTextureType2D, 1, 1, 2);
        clearZeta(into, 0, 0, 0, 2);
        clearZeta(into, 1, 0, 0, 2);
        resolveZeta("4x Stencil8 into level 1 of a 64x64", kS8, into, 1, 0, MTLMultisampleDepthResolveFilterSample0,
                    true);
        zetaOf("into (2)", into, 0);
        zetaOf("into (2)", into, 1);
    }
    {
        id<MTLTexture> into = make(kD32S8, 32, 32, MTLTextureType2DArray, 1, 2);
        clearZeta(into, 0, 0, 0.9, 2);
        clearZeta(into, 0, 1, 0.9, 2);
        resolveZeta("4x Depth32Float_Stencil8 stencil into slice 1", kD32S8, into, 0, 1,
                    MTLMultisampleDepthResolveFilterSample0, true);
        resolveZeta("4x Depth32Float_Stencil8 depth (min) into slice 1", kD32S8, into, 0, 1,
                    MTLMultisampleDepthResolveFilterMin, false);
        zetaOf("into (.9/2)", into, 0, 0);
        zetaOf("into (.9/2)", into, 0, 1);
    }
    {
        id<MTLTexture> into = make(kD32, 32, 32, MTLTextureType2D, 1, 1, 1, MTLStorageModeMemoryless);
        resolveZeta("4x Depth32Float into a memoryless Depth32Float", kD32, into, 0, 0,
                    MTLMultisampleDepthResolveFilterMax, false);
        id<MTLTexture> s = make(kS8, 32, 32, MTLTextureType2D, 1, 1, 1, MTLStorageModeMemoryless);
        resolveZeta("4x Stencil8 into a memoryless Stencil8", kS8, s, 0, 0, MTLMultisampleDepthResolveFilterSample0,
                    true);
    }
    {
        id<MTLTexture> into = make(kD32);
        clearZeta(into, 0, 0, 0.9, 0);
        resolveZeta("4x Depth32Float_Stencil8 depth (max) into a Depth32Float", kD32S8, into, 0, 0,
                    MTLMultisampleDepthResolveFilterMax, false);
        zetaOf("into (.9)", into);
        id<MTLTexture> into2 = make(kD32S8);
        clearZeta(into2, 0, 0, 0.9, 2);
        resolveZeta("4x Depth32Float (max) into a Depth32Float_Stencil8", kD32, into2, 0, 0,
                    MTLMultisampleDepthResolveFilterMax, false);
        zetaOf("into (.9/2)", into2);
        id<MTLTexture> into3 = make(kS8);
        clearZeta(into3, 0, 0, 0, 2);
        resolveZeta("4x Depth32Float_Stencil8 stencil into a Stencil8", kD32S8, into3, 0, 0,
                    MTLMultisampleDepthResolveFilterSample0, true);
        zetaOf("into (2)", into3);
    }
}

}  // namespace passforms
