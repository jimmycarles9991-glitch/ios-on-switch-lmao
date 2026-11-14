// SPDX-License-Identifier: MIT
// hw_test_app's 2D-engine probe ops through the raw facade (RawMetalProbe.mm switchRawProbeEngine:): the textures an op
// reads and writes are uses of its blit encoder in the command buffer's plan, as a copy's are, so a new texture's first
// contents (RawMetalFresh.mm) are written ahead of that encoder, never over what the op wrote.
#include "raw_facade_harness.h"

namespace {

id<MTLTexture> target()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [facade::device() newTextureWithDescriptor:d];
}

NSString *engine(id<MTLCommandBuffer> cb, NSDictionary *op)
{
    return ((NSString * (*)(id, SEL, NSDictionary *)) objc_msgSend)(cb, NSSelectorFromString(@"switchRawProbeEngine:"),
                                                                    op);
}

uint8_t access(RMCommandPlan &plan, id<MTLTexture> t)
{
    plan.prepareRange(0, plan.size());
    uint8_t found = 0;
    plan.forEachResource(0, plan.size(), RMResourceKind::Texture, [&](const RMRangeResource &use) {
        if (use.object == (__bridge void *)t)
            found |= use.access;
    });
    return found;
}

TEST(FacadeProbe, AScaleNamesItsTexturesInItsBlitEncoder)
{
    @autoreleasepool {
        id<MTLTexture> read = target(), written = target();
        ASSERT_NE(read, nil);
        ASSERT_NE(written, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        const uint64_t step = 1ull << 32;
        EXPECT_EQ(engine(
                      cb, @{
                          @"kind" : @"scale",
                          @"read" : read,
                          @"written" : written,
                          @"du" : @(step),
                          @"dv" : @(step),
                          @"sx" : @(step / 2),
                          @"sy" : @(step / 2),
                          @"corner" : @1,
                      }),
                  nil);
        [blit endEncoding];
        RMCommandPlan &plan = ((RMCommandBuffer *)cb)->_plan;
        EXPECT_TRUE(access(plan, written) & raw::ResourceUse::Write);
        EXPECT_TRUE(access(plan, read) & raw::ResourceUse::Read);
        EXPECT_FALSE(access(plan, read) & raw::ResourceUse::Write);
    }
}

TEST(FacadeProbe, AFillNamesItsTarget)
{
    @autoreleasepool {
        id<MTLTexture> written = target();
        ASSERT_NE(written, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        EXPECT_EQ(engine(
                      cb, @{
                          @"kind" : @"fill",
                          @"written" : written,
                          @"dx" : @1,
                          @"dy" : @1,
                          @"dw" : @14,
                          @"dh" : @14,
                          @"colorFormat" : @0xcf,
                          @"color0" : @0xff336699u,
                      }),
                  nil);
        [blit endEncoding];
        EXPECT_TRUE(access(((RMCommandBuffer *)cb)->_plan, written) & raw::ResourceUse::Write);
    }
}

std::vector<uint32_t> gWords;

void keepWords(const uint32_t *words, const TegraGM20BAbiPush *pushes, uint32_t count)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++)
        total += pushes[i].dwords;
    gWords.insert(gWords.end(), words, words + total);
}

// A private texture may move to plain memory between the op's record and its encode (raw_private_comp.h): the op writes
// where the texture is at encode
TEST(FacadeProbe, AScaleWritesWhereItsTextureIsAtEncode)
{
    @autoreleasepool {
        id<MTLTexture> read = target(), written = target(), moved = target();
        ASSERT_NE(moved, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        EXPECT_EQ(engine(cb, @{@"kind" : @"scale", @"read" : read, @"written" : written}), nil);
        [blit endEncoding];
        RMTexture *w = (RMTexture *)written;
        const uint64_t before = w->_raw.va, after = ((RMTexture *)moved)->_raw.va;
        w->_raw.va = after;
        gWords.clear();
        kernel_model::checkJob = keepWords;
        [cb commit];
        [cb waitUntilCompleted];
        kernel_model::checkJob = kernel_model::checkJobMethods;
        w->_raw.va = before;
        EXPECT_NE(std::find(gWords.begin(), gWords.end(), uint32_t(after)), gWords.end());
        EXPECT_EQ(std::find(gWords.begin(), gWords.end(), uint32_t(before)), gWords.end());
    }
}

}  // namespace
