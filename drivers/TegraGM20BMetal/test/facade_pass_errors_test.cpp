// SPDX-License-Identifier: MIT
// Render passes Metal's validation layer stops, nothing of them on m2v (TegraGM20BMetalPassErrors.h): those an M4 Max runs
// with a defined result run so on the driver; those it runs into garbage or a GPU fault fail their command buffer
// as it is recorded, the predicate named in its path. A target of a format the GM20B renders none of whose stand-in
// states the pack lacks fails as it is encoded; a YCbCr texture with no stand-in fails a pass into it; a resolve
// texture beside a store that resolves nothing is left alone; a state of a YCbCr format no target texture is made of
// is the driver's own. A pass that differs from the M4 Max's is counted skipped at each call.
#include "facade_harness.h"
#include "../attachment_target.h"
#include "../samples.h"
#include "../ycbcr_store.h"
#include "bazel/testing/fresh_process.h"

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
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

id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, NSUInteger samples = 1,
                       MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
                       MTLStorageMode storage = MTLStorageModePrivate)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    d.storageMode = storage;
    d.usage = usage;
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    return [device newTextureWithDescriptor:d];
}

MTLRenderPassDescriptor *colourPass(id<MTLTexture> t)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    return p;
}

MTLRenderPassDescriptor *depthPass(id<MTLTexture> t, MTLLoadAction load, MTLStoreAction store)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.depthAttachment.texture = t;
    p.depthAttachment.loadAction = load;
    p.depthAttachment.storeAction = store;
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

void expectRaw(id<MTLDevice> device, MTLRenderPassDescriptor *p)
{
    NSDictionary *before = counters(device);
    id<MTLCommandBuffer> cb = run(device, p);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    EXPECT_EQ(path(cb), "gm20b");
    NSDictionary *after = counters(device);
    EXPECT_TRUE(failedMoved(before, after).empty()) << text(after);
    EXPECT_NE(after[@"failed"], nil) << text(after);
}

// A pass of `p`, nothing drawn: its command buffer failed Internal on the driver, `pred` named in its path and
// counted once under it
void expectFailed(id<MTLDevice> device, MTLRenderPassDescriptor *p, const char *pred)
{
    NSDictionary *before = counters(device);
    id<MTLCommandBuffer> cb = run(device, p);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
    EXPECT_EQ(path(cb), std::string("error:") + pred);
    EXPECT_EQ(failedMoved(before, counters(device)), failedOnce(pred)) << text(counters(device));
}

id<MTLDevice> openPacked()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
}

// An M4 Max keeps none of a block-format attachment's writes, and keeps a one-sample Depth32Float's bits as its colour
// twin's (zeta_colour.h); a Stencil8 as a colour attachment fails
void colourAttachmentsOfFormatsNoColourAttachmentTakes()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> astc = texture(device, MTLPixelFormatASTC_4x4_LDR, 1, MTLTextureUsageShaderRead);
    id<MTLTexture> depth = texture(device, MTLPixelFormatDepth32Float);
    id<MTLTexture> stencil = texture(device, MTLPixelFormatStencil8);
    ASSERT_TRUE(astc != nil && depth != nil && stencil != nil);
    expectRaw(device, colourPass(astc));
    expectRaw(device, colourPass(depth));
    expectFailed(device, colourPass(stencil), "pass.fmt");
}

// An M4 Max keeps none of the writes to a level or slice past its texture's, takes a plain texture's slice as 0 and
// draws the layers an attachment has of a pass of more; a one-sample colour beside a four-sample depth renders
// (mixed_samples.h); two other sample counts fail
void partsPastTheirTextures()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> plain = texture(device, MTLPixelFormatBGRA8Unorm);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 2;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> array = [device newTextureWithDescriptor:d];
    id<MTLTexture> depth4 = texture(device, MTLPixelFormatDepth32Float, 4);
    id<MTLTexture> two = texture(device, MTLPixelFormatBGRA8Unorm, 2);
    ASSERT_TRUE(plain != nil && array != nil && depth4 != nil && two != nil);
    MTLRenderPassDescriptor *level = colourPass(plain);
    level.colorAttachments[0].level = 3;
    expectRaw(device, level);
    MTLRenderPassDescriptor *slice = colourPass(plain);
    slice.colorAttachments[0].slice = 1;
    expectRaw(device, slice);
    MTLRenderPassDescriptor *layered = colourPass(array);
    layered.colorAttachments[0].slice = 1;
    layered.renderTargetArrayLength = 2;
    expectRaw(device, layered);
    MTLRenderPassDescriptor *mixed = colourPass(plain);
    mixed.depthAttachment.texture = depth4;
    expectRaw(device, mixed);
    MTLRenderPassDescriptor *samples = colourPass(two);
    samples.depthAttachment.texture = depth4;
    expectFailed(device, samples, "pass.level");
}

// An M4 Max copies a one-sample attachment's resolve; a resolve across formats fails
void resolvesMetalStopsAndOneLeftAlone()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> plain = texture(device, MTLPixelFormatBGRA8Unorm);
    id<MTLTexture> into = texture(device, MTLPixelFormatBGRA8Unorm);
    id<MTLTexture> four = texture(device, MTLPixelFormatBGRA8Unorm, 4);
    id<MTLTexture> other = texture(device, MTLPixelFormatRGBA16Float);
    ASSERT_TRUE(plain != nil && other != nil && four != nil && into != nil);
    MTLRenderPassDescriptor *single = colourPass(plain);
    single.colorAttachments[0].resolveTexture = into;
    single.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    expectRaw(device, single);
    MTLRenderPassDescriptor *otherFormat = colourPass(four);
    otherFormat.colorAttachments[0].resolveTexture = other;
    otherFormat.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    expectFailed(device, otherFormat, "pass.level");
    MTLRenderPassDescriptor *stored = colourPass(plain);
    stored.colorAttachments[0].resolveTexture = into;
    expectRaw(device, stored);
}

// An M4 Max keeps Depth32Float bits in a one-sample colour texture of 4-byte texels as the depth (zeta_colour.h); a
// Stencil8 or a colour of another texel size as the depth fails, and a one-sample depth's resolve faults Apple's GPU
void depthAndStencilAttachmentsMetalStops()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> colour = texture(device, MTLPixelFormatBGRA8Unorm);
    id<MTLTexture> wide = texture(device, MTLPixelFormatRGBA16Float);
    id<MTLTexture> stencil = texture(device, MTLPixelFormatStencil8);
    id<MTLTexture> depth = texture(device, MTLPixelFormatDepth32Float);
    id<MTLTexture> into = texture(device, MTLPixelFormatDepth32Float);
    id<MTLTexture> memoryless =
        texture(device, MTLPixelFormatDepth32Float, 1, MTLTextureUsageRenderTarget, MTLStorageModeMemoryless);
    ASSERT_TRUE(colour != nil && wide != nil && stencil != nil && depth != nil && into != nil && memoryless != nil);
    expectRaw(device, depthPass(colour, MTLLoadActionClear, MTLStoreActionStore));
    expectFailed(device, depthPass(wide, MTLLoadActionClear, MTLStoreActionStore), "pass.ds");
    expectFailed(device, depthPass(stencil, MTLLoadActionClear, MTLStoreActionStore), "pass.ds");
    MTLRenderPassDescriptor *resolved = depthPass(depth, MTLLoadActionClear, MTLStoreActionMultisampleResolve);
    resolved.depthAttachment.resolveTexture = into;
    expectFailed(device, resolved, "pass.ds");
}

// An M4 Max loads a memoryless depth or stencil as zeros and stores nothing of it, and keeps the aspect of a
// Depth32Float_Stencil8 a pass does not attach
void depthAndStencilAttachmentsAppleRuns()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> memoryless =
        texture(device, MTLPixelFormatDepth32Float, 1, MTLTextureUsageRenderTarget, MTLStorageModeMemoryless);
    id<MTLTexture> both = texture(device, MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_TRUE(memoryless != nil && both != nil);
    expectRaw(device, depthPass(memoryless, MTLLoadActionLoad, MTLStoreActionStore));
    expectRaw(device, depthPass(both, MTLLoadActionLoad, MTLStoreActionStore));
    MTLRenderPassDescriptor *stencilAlone = [MTLRenderPassDescriptor renderPassDescriptor];
    stencilAlone.stencilAttachment.texture = both;
    stencilAlone.stencilAttachment.loadAction = MTLLoadActionClear;
    stencilAlone.stencilAttachment.storeAction = MTLStoreActionStore;
    expectRaw(device, stencilAlone);
}

// Metal's validation stops it, and an M4 Max resolves it in the attachment's encoding in the destination's order
void aResolveAcrossTheEightBitTwinsIsRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{gm20b::resolveState(80, 4), MTLPixelFormatBGRA8Unorm}}));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> four = texture(device, MTLPixelFormatBGRA8Unorm, 4);
    const MTLTextureUsage usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> into = texture(device, MTLPixelFormatRGBA8Unorm, 1, usage, MTLStorageModeShared);
    ASSERT_TRUE(four != nil && into != nil);
    MTLRenderPassDescriptor *p = colourPass(four);
    p.colorAttachments[0].resolveTexture = into;
    p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    expectRaw(device, p);
    // an M4 Max resolves into a texture of another sample count too, each sample the resolved colour
    id<MTLTexture> two = texture(device, MTLPixelFormatBGRA8Unorm, 2);
    ASSERT_TRUE(two != nil);
    MTLRenderPassDescriptor *otherCount = colourPass(four);
    otherCount.colorAttachments[0].resolveTexture = two;
    otherCount.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    expectRaw(device, otherCount);
}

// A1BGR5Unorm renders through an RGBA32Float stand-in whose load and store states this pack lacks
void aStandInTargetWithoutItsStatesFailsAsItIsEncoded()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> target = texture(device, MTLPixelFormatA1BGR5Unorm);
    ASSERT_TRUE(target != nil);
    expectFailed(device, colourPass(target), "pass.fmt");
}

void aYcbcrTextureWithNoStandInFailsAPassIntoIt()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float}}));
    ASSERT_TRUE(device != nil);
    CVPixelBufferRef pixels = nullptr;
    NSDictionary *attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    ASSERT_EQ(CVPixelBufferCreate(kCFAllocatorDefault, 64, 48, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange,
                                  (__bridge CFDictionaryRef)attributes, &pixels),
              kCVReturnSuccess);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixels);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), CFSTR("ITU_R_709_2"));
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(502) width:64 height:48 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> planar = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(planar != nil);
    expectFailed(device, colourPass(planar), "enc.planarrt");
    CVPixelBufferRelease(pixels);
}

void aYcbcrStateOfAFormatWithNoTargetIsRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormat(gm20b::kYcbcrTargetStandIn))));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormat(501)) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_TRUE([((NSNumber * (*)(id, SEL)) objc_msgSend)(state, sel_registerName("switchPackHit")) boolValue]);
}

// A resolve of signed normalized codes by the 2D engine into a level of a chain, and over a render area smaller than its
// target
void codesResolvesOfAnyShapeAreRaws()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> four = texture(device, MTLPixelFormatRGBA8Snorm, 4);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Snorm
                                                                                 width:128
                                                                                height:128
                                                                             mipmapped:YES];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> chain = [device newTextureWithDescriptor:d];
    id<MTLTexture> into = texture(device, MTLPixelFormatRGBA8Snorm);
    ASSERT_TRUE(four != nil && chain != nil && into != nil);
    MTLRenderPassDescriptor *level = colourPass(four);
    level.colorAttachments[0].resolveTexture = chain;
    level.colorAttachments[0].resolveLevel = 1;
    level.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    expectRaw(device, level);
    MTLRenderPassDescriptor *area = colourPass(four);
    area.colorAttachments[0].resolveTexture = into;
    area.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    area.renderTargetWidth = 40;
    area.renderTargetHeight = 24;
    expectRaw(device, area);
}

// A view of some layers of a mipmapped 2D array is no target as a whole: its slice is, its layers its parent's whole
// chain apart
void aSliceOfAViewOfAMipmappedArrayIsRaws()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:YES];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> array = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(array != nil);
    id<MTLTexture> view = [array newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                   textureType:MTLTextureType2DArray
                                                        levels:NSMakeRange(0, 1)
                                                        slices:NSMakeRange(1, 2)];
    ASSERT_TRUE(view != nil);
    MTLRenderPassDescriptor *p = colourPass(view);
    p.colorAttachments[0].slice = 1;
    expectRaw(device, p);
}

// The pack's states a multisampled A1BGR5Unorm pass and its resolve draw: its stand-in's per-sample fill and store, the
// RGBA32Float resolve of the stand-in, and the one-sample store of the resolve's stand-in
std::vector<std::pair<std::string, MTLPixelFormat>> a1bgr5States(uint32_t samples)
{
    const auto view = MTLPixelFormat(gm20b::formatStoreView(gm20b::kA1Bgr5Unorm));
    std::vector<std::pair<std::string, MTLPixelFormat>> states = {
        {gm20b::formatLoadState(gm20b::kA1Bgr5Unorm), MTLPixelFormatRGBA32Float},
        {gm20b::formatStoreState(gm20b::kA1Bgr5Unorm), view}};
    if (samples > 1) {
        states.push_back({gm20b::formatLoadState(gm20b::kA1Bgr5Unorm, samples), MTLPixelFormatRGBA32Float});
        states.push_back({gm20b::formatStoreState(gm20b::kA1Bgr5Unorm, samples), view});
        states.push_back({gm20b::resolveState(125, samples), MTLPixelFormatRGBA32Float});
    }
    return states;
}

// A multisampled target of a format the GM20B renders none of renders through a stand-in of its samples, stored a sample
// at a time, and resolves through a stand-in of the resolve texture's
void aMultisampledStandInTargetAndItsResolveAreRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", a1bgr5States(4)));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> four = texture(device, MTLPixelFormatA1BGR5Unorm, 4);
    id<MTLTexture> into = texture(device, MTLPixelFormatA1BGR5Unorm);
    ASSERT_TRUE(four != nil && into != nil);
    expectRaw(device, colourPass(four));
    MTLRenderPassDescriptor *resolved = colourPass(four);
    resolved.colorAttachments[0].resolveTexture = into;
    resolved.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    expectRaw(device, resolved);
}

// Without its per-sample states the pass fails as it is encoded, nothing of it on m2v
void aMultisampledStandInWithoutItsStatesFails()
{
    id<MTLDevice> device = openPacked();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> four = texture(device, MTLPixelFormatA1BGR5Unorm, 4);
    ASSERT_TRUE(four != nil);
    expectFailed(device, colourPass(four), "pass.fmt");
}

// RGB9E5Float resolves by its own state: one sample of the samples' decoded mean
void anRgb9e5ResolveIsItsOwnStates()
{
    const auto view = MTLPixelFormat(gm20b::formatStoreView(gm20b::kRgb9e5Float));
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{gm20b::formatLoadState(gm20b::kRgb9e5Float, 2), MTLPixelFormatRGBA32Float},
                                           {gm20b::formatStoreState(gm20b::kRgb9e5Float, 2), view},
                                           {gm20b::formatResolveState(gm20b::kRgb9e5Float), MTLPixelFormatRGBA32Float},
                                           {gm20b::formatLoadState(gm20b::kRgb9e5Float), MTLPixelFormatRGBA32Float},
                                           {gm20b::formatStoreState(gm20b::kRgb9e5Float), view}}));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> two = texture(device, MTLPixelFormatRGB9E5Float, 2);
    id<MTLTexture> into = texture(device, MTLPixelFormatRGB9E5Float);
    ASSERT_TRUE(two != nil && into != nil);
    MTLRenderPassDescriptor *p = colourPass(two);
    p.colorAttachments[0].resolveTexture = into;
    p.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    expectRaw(device, p);
}

// A layered pass into a 2D array of such a format: a stand-in of its layers, each filled and stored
void aLayeredStandInPassIsRaws()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", a1bgr5States(1)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatA1BGR5Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 3;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> array = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(array != nil);
    MTLRenderPassDescriptor *p = colourPass(array);
    p.colorAttachments[0].loadAction = MTLLoadActionLoad;
    p.colorAttachments[0].slice = 1;
    p.renderTargetArrayLength = 2;
    expectRaw(device, p);
}

// renderTargetArrayLength past a plain array's layers draws the layers it has, as an M4 Max draws them: no skip. Beside a
// stand-in array of fewer layers, the pass draws the layers both have where the M4 Max draws the plain array's every
// layer: a skip of pass.level at each call
void layersPastBesideAStandInAreCountedSkipped()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", a1bgr5States(1)));
    ASSERT_TRUE(device != nil);
    const auto array = [&](MTLPixelFormat format, NSUInteger layers) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = layers;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        return [device newTextureWithDescriptor:d];
    };
    id<MTLTexture> plain = array(MTLPixelFormatBGRA8Unorm, 3), standIn = array(MTLPixelFormatA1BGR5Unorm, 2);
    ASSERT_TRUE(plain != nil && standIn != nil);
    MTLRenderPassDescriptor *alone = colourPass(plain);
    alone.colorAttachments[0].slice = 1;
    alone.renderTargetArrayLength = 3;
    NSDictionary *before = counters(device);
    expectRaw(device, alone);
    EXPECT_TRUE(skippedMoved(before, counters(device)).empty()) << text(counters(device));
    MTLRenderPassDescriptor *beside = colourPass(plain);
    beside.colorAttachments[1].texture = standIn;
    beside.colorAttachments[1].loadAction = MTLLoadActionLoad;
    beside.colorAttachments[1].storeAction = MTLStoreActionStore;
    beside.renderTargetArrayLength = 3;
    before = counters(device);
    expectRaw(device, beside);
    expectRaw(device, beside);
    const std::map<std::string, long long> eachCall = {{"skipped", 2}, {"skipped.pass.level", 2}};
    EXPECT_EQ(skippedMoved(before, counters(device)), eachCall) << text(counters(device));
}

}  // namespace

TEST(GM20BFacadePassErrors, ASliceOfAViewOfAMipmappedArrayIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSliceOfAViewOfAMipmappedArrayIsRaws);
}

TEST(GM20BFacadePassErrors, CodesResolvesOfAnyShapeAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(codesResolvesOfAnyShapeAreRaws);
}

TEST(GM20BFacadePassErrors, AMultisampledStandInTargetAndItsResolveAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aMultisampledStandInTargetAndItsResolveAreRaws);
}

TEST(GM20BFacadePassErrors, AMultisampledStandInWithoutItsStatesFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(aMultisampledStandInWithoutItsStatesFails);
}

TEST(GM20BFacadePassErrors, AnRgb9e5ResolveIsItsOwnStates)
{
    switch_ios::test::ExpectPassesInFreshProcess(anRgb9e5ResolveIsItsOwnStates);
}

TEST(GM20BFacadePassErrors, ALayeredStandInPassIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aLayeredStandInPassIsRaws);
}

TEST(GM20BFacadePassErrors, LayersPastBesideAStandInAreCountedSkippedAtEachCall)
{
    switch_ios::test::ExpectPassesInFreshProcess(layersPastBesideAStandInAreCountedSkipped);
}

TEST(GM20BFacadePassErrors, ABlockColourAttachmentIsDroppedADepthOneIsItsColourTwinAndAStencilOneFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(colourAttachmentsOfFormatsNoColourAttachmentTakes);
}

TEST(GM20BFacadePassErrors, PartsAndLayersPastTheirTexturesAreDroppedMixedSamplesRenderAndOtherCountsFail)
{
    switch_ios::test::ExpectPassesInFreshProcess(partsPastTheirTextures);
}

TEST(GM20BFacadePassErrors, ResolvesAppleRunsAreRawsAndOneAcrossFormatsFails)
{
    switch_ios::test::ExpectPassesInFreshProcess(resolvesMetalStopsAndOneLeftAlone);
}

TEST(GM20BFacadePassErrors, AResolveAcrossTheEightBitTwinsAndIntoAnotherSampleCountIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aResolveAcrossTheEightBitTwinsIsRaws);
}

TEST(GM20BFacadePassErrors, AFourByteColourAsDepthIsRawsAndOtherDepthAttachmentsMetalStopsFail)
{
    switch_ios::test::ExpectPassesInFreshProcess(depthAndStencilAttachmentsMetalStops);
}

TEST(GM20BFacadePassErrors, DepthAndStencilAttachmentsAppleRunsAreRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(depthAndStencilAttachmentsAppleRuns);
}

TEST(GM20BFacadePassErrors, AStandInTargetWithoutItsStatesFailsAsItIsEncoded)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStandInTargetWithoutItsStatesFailsAsItIsEncoded);
}

TEST(GM20BFacadePassErrors, AYcbcrTextureWithNoStandInFailsAPassIntoIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(aYcbcrTextureWithNoStandInFailsAPassIntoIt);
}

TEST(GM20BFacadePassErrors, AYcbcrStateOfAFormatWithNoTargetIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aYcbcrStateOfAFormatWithNoTargetIsRaws);
}
