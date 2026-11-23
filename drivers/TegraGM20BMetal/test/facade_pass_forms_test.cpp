// SPDX-License-Identifier: MIT
// Render passes an M4 Max runs with API validation off (harness/passformsoracle.mm, norenderoracle.mm) that the pass-begin sites
// sent to m2v: each is the driver's, or fails its command buffer as Apple's GPU fails it, and none opens m2v. Colour: a
// plane of a 3D texture made without render-target usage (several planes a block), an RG8Unorm_sRGB target, a texture
// buffer's row (one past 16384 texels fails Internal). Depth/stencil: the aspects in two textures, levels or slices of one,
// across Depth32Float_Stencil8, Depth32Float and Stencil8, of two extents, and through an X32_Stencil8 view; resolves into a
// level, a slice and another zeta format (one into a memoryless texture fails InvalidResource).
#include "facade_harness.h"
#include "../record.h"
#include "../samples.h"
#include "../stencil_bits.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <string>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

std::string path(id<MTLCommandBuffer> cb)
{
    NSString *p = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
    return p.UTF8String ?: "";
}

id<MTLTexture> make(id<MTLDevice> device, MTLPixelFormat format, NSUInteger w = 64, NSUInteger h = 64,
                    MTLTextureType type = MTLTextureType2D, NSUInteger samples = 1, NSUInteger layers = 1,
                    NSUInteger levels = 1, MTLStorageMode storage = MTLStorageModePrivate,
                    MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = format;
    d.textureType = type;
    d.width = w;
    d.height = h;
    d.depth = type == MTLTextureType3D ? 8 : 1;
    d.sampleCount = samples;
    d.arrayLength = layers;
    d.mipmapLevelCount = levels;
    d.storageMode = storage;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

struct Aspect {
    id<MTLTexture> t;
    NSUInteger level = 0, slice = 0;
};

MTLRenderPassDescriptor *zetaPass(Aspect d, Aspect s)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.depthAttachment.texture = d.t;
    p.depthAttachment.level = d.level;
    p.depthAttachment.slice = d.slice;
    p.depthAttachment.loadAction = MTLLoadActionLoad;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.stencilAttachment.texture = s.t;
    p.stencilAttachment.level = s.level;
    p.stencilAttachment.slice = s.slice;
    p.stencilAttachment.loadAction = MTLLoadActionClear;
    p.stencilAttachment.storeAction = MTLStoreActionStore;
    return p;
}

MTLRenderPassDescriptor *colourPass(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    return p;
}

id<MTLCommandBuffer> run(id<MTLDevice> device, MTLRenderPassDescriptor *p)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

void expectRaw(id<MTLDevice> device, MTLRenderPassDescriptor *p, const char *what)
{
    id<MTLCommandBuffer> cb = run(device, p);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << what << ": " << text(cb.error);
    EXPECT_EQ(path(cb), "gm20b") << what;
}

void expectFailed(id<MTLDevice> device, MTLRenderPassDescriptor *p, const char *pred, MTLCommandBufferError code,
                  const char *what)
{
    id<MTLCommandBuffer> cb = run(device, p);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError) << what;
    EXPECT_EQ(cb.error.code, code) << what << ": " << text(cb.error);
    EXPECT_EQ(path(cb), std::string("error:") + pred) << what;
}

id<MTLDevice> openPacked()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
}

// The pack's own state `name` (samples.h, stencil_bits.h), of key fields `fields`, on the open device
bool ownState(const char *name, const char *fields)
{
    gm20b::RuntimeRecord r;
    r.keyString = fields;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    return gRM->raw->addRecord(r, false, why) != nullptr;
}

// A plane of a 3D texture made with ShaderRead usage alone (an M4 Max renders it: norenderoracle.mm) and a texture buffer's
// row
void coloursAppleRendersAreRaws()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> volume = make(device, MTLPixelFormatRGBA8Unorm, 16, 16, MTLTextureType3D, 1, 1, 1,
                                 MTLStorageModePrivate, MTLTextureUsageShaderRead);
    id<MTLBuffer> rows = [device newBufferWithLength:20000 * 4 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *row =
        [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:256
                                                     resourceOptions:MTLResourceStorageModeShared
                                                               usage:MTLTextureUsageRenderTarget];
    id<MTLTexture> buffer = [rows newTextureWithDescriptor:row offset:0 bytesPerRow:256 * 4];
    row.width = 20000;
    id<MTLTexture> wide = [rows newTextureWithDescriptor:row offset:0 bytesPerRow:20000 * 4];
    ASSERT_TRUE(volume != nil && buffer != nil && wide != nil);
    MTLRenderPassDescriptor *plane = colourPass(volume);
    plane.colorAttachments[0].depthPlane = 5;
    expectRaw(device, plane, "a plane inside a block of several");
    expectRaw(device, colourPass(buffer), "a texture buffer of 256 texels");
    expectFailed(device, colourPass(wide), "pass.fmt", MTLCommandBufferErrorInternal, "a texture buffer of 20000");
}

// An RG8Unorm_sRGB target through an RGBA8Unorm_sRGB stand-in, its first two bytes moved by the copy engine (an M4 Max stores
// RGBA8Unorm_sRGB's first two bytes exactly in draws, clears and blends: passformsoracle.mm rg8_srgb)
void anRg8SrgbTargetIsRaws()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> pair = make(device, MTLPixelFormatRG8Unorm_sRGB);
    ASSERT_TRUE(pair != nil);
    expectRaw(device, colourPass(pair), "RG8Unorm_sRGB");
    MTLRenderPassDescriptor *cleared = colourPass(pair);
    cleared.colorAttachments[0].loadAction = MTLLoadActionClear;
    expectRaw(device, cleared, "RG8Unorm_sRGB cleared");
}

// Each aspect in its own texture, level or slice, of its own format and extent (passformsoracle.mm zeta_two_*, mixed_pairs,
// pair_sizes, stencil_view)
void zetaAspectsApartAreRaws()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> a = make(device, MTLPixelFormatDepth32Float_Stencil8),
                   b = make(device, MTLPixelFormatDepth32Float_Stencil8);
    id<MTLTexture> levels = make(device, MTLPixelFormatDepth32Float_Stencil8, 64, 64, MTLTextureType2D, 1, 1, 2);
    id<MTLTexture> slices = make(device, MTLPixelFormatDepth32Float_Stencil8, 64, 64, MTLTextureType2DArray, 1, 2);
    id<MTLTexture> depth = make(device, MTLPixelFormatDepth32Float),
                   small = make(device, MTLPixelFormatStencil8, 32, 32);
    id<MTLTexture> stencil = make(device, MTLPixelFormatStencil8);
    ASSERT_TRUE(a && b && levels && slices && depth && small && stencil);
    id<MTLTexture> view = [a newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    ASSERT_TRUE(view != nil);
    expectRaw(device, zetaPass({a}, {b}), "two Depth32Float_Stencil8 textures");
    expectRaw(device, zetaPass({levels, 0}, {levels, 1}), "two levels of one");
    expectRaw(device, zetaPass({slices, 0, 0}, {slices, 0, 1}), "two slices of one");
    expectRaw(device, zetaPass({a}, {stencil}), "Depth32Float_Stencil8 beside a Stencil8");
    expectRaw(device, zetaPass({depth}, {b}), "Depth32Float beside a Depth32Float_Stencil8's stencil");
    expectRaw(device, zetaPass({depth}, {small}), "a Depth32Float and a smaller Stencil8");
    expectRaw(device, zetaPass({a}, {view}), "a texture beside its X32_Stencil8 view");
    expectRaw(device, zetaPass({nil}, {view}), "an X32_Stencil8 view alone");
}

// Resolves into a level, a slice and another zeta format (passformsoracle.mm zeta_resolves), by the pack's Depth32Float
// maximum depth resolve and its Depth32Float_Stencil8 stencil bits of four samples; one into a memoryless texture fails as
// an M4 Max fails it
void zetaResolves()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    ASSERT_TRUE(ownState(gm20b::depthResolveState(MTLPixelFormatDepth32Float, MTLMultisampleDepthResolveFilterMax),
                         "samples=1 ds=252/0 "));
    ASSERT_TRUE(
        ownState(gm20b::stencilBitsState(gm20b::kZetaFormat, 4, gm20b::StencilSample::Zero), "samples=1 ds=260/260 "));
    id<MTLTexture> four = make(device, MTLPixelFormatDepth32Float, 32, 32, MTLTextureType2DMultisample, 4);
    id<MTLTexture> fourBoth = make(device, MTLPixelFormatDepth32Float_Stencil8, 32, 32, MTLTextureType2DMultisample, 4);
    id<MTLTexture> levels = make(device, MTLPixelFormatDepth32Float, 64, 64, MTLTextureType2D, 1, 1, 2);
    id<MTLTexture> slices = make(device, MTLPixelFormatDepth32Float, 32, 32, MTLTextureType2DArray, 1, 2);
    id<MTLTexture> plain = make(device, MTLPixelFormatDepth32Float, 32, 32),
                   s8 = make(device, MTLPixelFormatStencil8, 32, 32);
    id<MTLTexture> memoryless = make(device, MTLPixelFormatDepth32Float, 32, 32, MTLTextureType2D, 1, 1, 1,
                                     MTLStorageModeMemoryless, MTLTextureUsageRenderTarget);
    ASSERT_TRUE(four && fourBoth && levels && slices && plain && s8 && memoryless);
    const auto resolved = [](id<MTLTexture> from, id<MTLTexture> into, NSUInteger level, NSUInteger slice,
                             bool stencil) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassAttachmentDescriptor *a = stencil ? (MTLRenderPassAttachmentDescriptor *)p.stencilAttachment
                                                       : (MTLRenderPassAttachmentDescriptor *)p.depthAttachment;
        a.texture = from;
        a.loadAction = MTLLoadActionClear;
        a.storeAction = MTLStoreActionMultisampleResolve;
        a.resolveTexture = into;
        a.resolveLevel = level;
        a.resolveSlice = slice;
        p.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterMax;
        return p;
    };
    expectRaw(device, resolved(four, levels, 1, 0, false), "a depth resolve into level 1");
    expectRaw(device, resolved(four, slices, 0, 1, false), "a depth resolve into slice 1");
    expectRaw(device, resolved(fourBoth, plain, 0, 0, false), "Depth32Float_Stencil8's depth into a Depth32Float");
    expectRaw(device, resolved(fourBoth, s8, 0, 0, true), "Depth32Float_Stencil8's stencil into a Stencil8");
    expectFailed(device, resolved(four, memoryless, 0, 0, false), "pass.ds", MTLCommandBufferErrorInvalidResource,
                 "a depth resolve into a memoryless texture");
}

}  // namespace

TEST(GM20BFacadePassForms, ColoursAppleRendersAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(coloursAppleRendersAreRaws);
}

TEST(GM20BFacadePassForms, AnRg8SrgbTargetIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(anRg8SrgbTargetIsRaws);
}

TEST(GM20BFacadePassForms, ZetaAspectsApartAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(zetaAspectsApartAreRaws);
}

TEST(GM20BFacadePassForms, ZetaResolvesIntoPartsAndFormatsAreRawsAndOneIntoMemorylessFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(zetaResolves);
}
