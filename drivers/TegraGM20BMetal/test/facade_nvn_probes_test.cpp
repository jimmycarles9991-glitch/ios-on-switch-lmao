// SPDX-License-Identifier: MIT
// What HW Test gpu.perf.knobs' and gpu.perf.nvn's arms read to prove their probes fired (apps/hwtest/gpu/GPURatesNvn.swift,
// GPURatesKnobs.swift), through the facade over the kernel model: the plugin's counters for the subtiling knob and the
// discards, the header's SECTOR_PROMOTION field, the facts line's kind= and the forms' taken flushes.
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <mutex>
#include <string>

namespace {

using scale_frames::opened;

constexpr uint32_t kNvnDiscard = 1u << 4;

id<MTLDevice> device()
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoTiledCache | TegraGM20BInfoCompression;
    }
    return opened().device;
}

NSString *probe(const char *selector, id argument)
{
    return ((NSString * (*)(id, SEL, id)) objc_msgSend)((id)device(), sel_registerName(selector), argument);
}

long long counter(NSString *key)
{
    return [harness::counters(device())[key] longLongValue];
}

// One pass of scale_frames' draws into `into`, its colour 1 `beside` stored DontCare, committed and completed
id<MTLCommandBuffer> drawn(id<MTLTexture> into, id<MTLTexture> beside = nil, NSString *forms = nil)
{
    id<MTLCommandBuffer> cb = [[device() newCommandQueue] commandBuffer];
    if (forms) {
        NSString *refused =
            ((NSString * (*)(id, SEL, id)) objc_msgSend)(cb, sel_registerName("switchMetalProbeForms:"), forms);
        EXPECT_EQ(refused, nil) << refused.UTF8String;
    }
    scale_frames::Shape shape;
    shape.passes = 1;
    shape.draws = 4;
    shape.targets = 1;
    scale_frames::pass(cb, opened().state, {into}, shape, 0, 0, beside);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    return cb;
}

id<MTLTexture> target()
{
    return scale_frames::targets(device(), scale_frames::Shape{})[0];
}

TEST(GM20BFacadeNvnProbes, EveryPassUnderTheSubtilingProbeWritesTheKnob)
{
    ASSERT_NE(device(), nil);
    id<MTLTexture> into = target();
    const long long before = counter(@"subtile_writes");
    drawn(into);
    EXPECT_EQ(counter(@"subtile_writes"), before) << "a pass wrote the knob with the probe off";
    probe("switchMetalProbeSubtilingKnob:", @(0x087F6080u));
    drawn(into);
    drawn(into);
    probe("switchMetalProbeSubtilingKnob:", @0);
    EXPECT_EQ(counter(@"subtile_writes") - before, 2);
}

TEST(GM20BFacadeNvnProbes, TheDiscardProbeDiscardsAColourNothingStores)
{
    ASSERT_NE(device(), nil);
    id<MTLTexture> into = target();
    id<MTLTexture> beside = scale_frames::scratch(device(), scale_frames::Shape{});
    const long long before = counter(@"nvn_discards");
    drawn(into, beside);
    EXPECT_EQ(counter(@"nvn_discards"), before) << "a discard with the probe off";
    probe("switchMetalProbeNvn:", @(kNvnDiscard));
    drawn(into, beside);
    probe("switchMetalProbeNvn:", @0);
    EXPECT_EQ(counter(@"nvn_discards") - before, 1) << "colour 1 alone: colour 0 is stored";
}

TEST(GM20BFacadeNvnProbes, APrivateTexturesHeaderTakesThePromotionItWasMadeUnder)
{
    ASSERT_NE(device(), nil);
    for (uint32_t mode : {1u, 2u, 3u, 0u}) {
        probe("switchMetalProbeSectorPromotion:", @(mode));
        id<MTLTexture> made = target();
        NSString *header = probe("switchMetalProbeHeader:", made);
        probe("switchMetalProbeSectorPromotion:", @0);
        ASSERT_NE(header, nil);
        NSArray<NSString *> *words = [header componentsSeparatedByString:@"."];
        ASSERT_EQ(words.count, 8u);
        const uint32_t word4 = uint32_t(strtoul(words[4].UTF8String, nullptr, 16));
        EXPECT_EQ(word4 >> 27 & 3, mode) << header.UTF8String;
    }
}

TEST(GM20BFacadeNvnProbes, TheFactsLineNamesAPlainTexturesKindAsNone)
{
    ASSERT_NE(device(), nil);
    probe("switchMetalProbePlainPrivate:", @YES);
    id<MTLTexture> plain = target();
    probe("switchMetalProbePlainPrivate:", @NO);
    NSString *facts = probe("switchMetalProbeFacts:", plain);
    EXPECT_TRUE([facts containsString:@"comptags=0"]) << facts.UTF8String;
    EXPECT_TRUE([facts containsString:@" kind=0x0 "]) << facts.UTF8String;
}

long compressed(NSString *key)
{
    return [harness::ask(device(), "switchMetalCompression")[key] longValue];
}

// The 2CBR probe turned off gives back the compressed memory made for its kinds: no texture takes it without the probe,
// and the compression tests after gpu.perf.nvn would meet a fuller pool
TEST(GM20BFacadeNvnProbes, TheCbrProbeTurningOffFreesWhatItsKindsMade)
{
    ASSERT_NE(device(), nil);
    const long before = compressed(@"held");
    probe("switchMetalProbeMs4Cbr:", @YES);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DMultisample;
    td.pixelFormat = MTLPixelFormatBGRA8Unorm;
    td.width = 640;
    td.height = 360;
    td.sampleCount = 4;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device() newTextureWithDescriptor:td];
    ASSERT_NE(target, nil);
    ASSERT_EQ(harness::compressionSettled(device()), 1);
    const long made = compressed(@"held");
    probe("switchMetalProbeMs4Cbr:", @NO);
    EXPECT_GT(made, before) << "no 2CBR memory was made for the target";
    EXPECT_EQ(compressed(@"held"), before) << "the 2CBR memory stayed held after the probe went off";
}

// Two passes on one target fused into one (pass_fusion.h): the second's DontCare store is the group's, so under the
// discard probe the group's end discards the target
TEST(GM20BFacadeNvnProbes, AFusedPassStoredDontCareDiscardsItsTarget)
{
    ASSERT_NE(device(), nil);
    id<MTLTexture> into = target();
    id<MTLCommandBuffer> cb = [[device() newCommandQueue] commandBuffer];
    for (MTLStoreAction store : {MTLStoreActionStore, MTLStoreActionDontCare}) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = into;
        d.colorAttachments[0].loadAction = store == MTLStoreActionStore ? MTLLoadActionClear : MTLLoadActionLoad;
        d.colorAttachments[0].storeAction = store;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
        [e setRenderPipelineState:opened().state];
        const float vertices[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
        const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float colour[4] = {0.25f, 0.5f, 0.75f, 1};
        [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
        [e setVertexBytes:transform length:sizeof transform atIndex:2];
        [e setFragmentBytes:colour length:sizeof colour atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    }
    const long fused = counter(@"fused_passes"), discards = counter(@"nvn_discards");
    probe("switchMetalProbeNvn:", @(kNvnDiscard));
    [cb commit];
    [cb waitUntilCompleted];
    probe("switchMetalProbeNvn:", @0);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    ASSERT_EQ(counter(@"fused_passes") - fused, 1) << "the two passes did not fuse";
    EXPECT_EQ(counter(@"nvn_discards") - discards, 1) << "the fused group kept its first pass's store";
}

TEST(GM20BFacadeNvnProbes, APassUnderNoflushFormsCountsTheFlushItLeftOut)
{
    ASSERT_NE(device(), nil);
    id<MTLTexture> into = target();
    id<MTLCommandBuffer> kept = drawn(into, nil, @"switch=nowfi pass=pixel");
    id<MTLCommandBuffer> dropped = drawn(into, nil, @"switch=nowfi_noflush pass=pixel");
    NSString *(*taken)(id, SEL) = (NSString * (*)(id, SEL)) objc_msgSend;
    NSString *keptTaken = taken(kept, sel_registerName("switchMetalProbeTaken"));
    NSString *droppedTaken = taken(dropped, sel_registerName("switchMetalProbeTaken"));
    EXPECT_TRUE([keptTaken containsString:@"flushes=0"]) << keptTaken.UTF8String;
    EXPECT_TRUE([droppedTaken containsString:@"flushes=1"]) << droppedTaken.UTF8String;
}

}  // namespace
