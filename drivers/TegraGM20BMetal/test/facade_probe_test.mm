// SPDX-License-Identifier: MIT
// hw_test_app's 2D-engine probe ops through the facade (TegraGM20BMetalProbe.mm switchMetalProbeEngine:): the textures an op
// reads and writes are uses of its blit encoder in the command buffer's plan, as a copy's are, so a new texture's first
// contents (TegraGM20BMetalFresh.mm) are written ahead of that encoder, never over what the op wrote. Its raw 3D-class
// methods refuse a process without SWITCH_GPU_PROBES_ENTITLEMENT.
#include "facade_harness.h"
#include "TegraGM20BGrPreemptABI.h"
#include "TegraGM20BPathsABI.h"

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
    return ((NSString * (*)(id, SEL, NSDictionary *))
                objc_msgSend)(cb, NSSelectorFromString(@"switchMetalProbeEngine:"), op);
}

uint8_t access(SwitchMetalCommandPlan &plan, id<MTLTexture> t)
{
    plan.prepareRange(0, plan.size());
    uint8_t found = 0;
    plan.forEachResource(0, plan.size(), SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &use) {
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
        SwitchMetalCommandPlan &plan = ((SwitchMetalCommandBuffer *)cb)->_plan;
        EXPECT_TRUE(access(plan, written) & gm20b::ResourceUse::Write);
        EXPECT_TRUE(access(plan, read) & gm20b::ResourceUse::Read);
        EXPECT_FALSE(access(plan, read) & gm20b::ResourceUse::Write);
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
        EXPECT_TRUE(access(((SwitchMetalCommandBuffer *)cb)->_plan, written) & gm20b::ResourceUse::Write);
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

// A private texture may move to plain memory between the op's record and its encode (private_comp.h): the op writes
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
        SwitchMetalTexture *w = (SwitchMetalTexture *)written;
        const uint64_t before = w->_raw.va, after = ((SwitchMetalTexture *)moved)->_raw.va;
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

TEST(FacadeProbe, ComputeCtaProbeSendsGrPreemptMethodForDeviceChannel)
{
    @autoreleasepool {
        id<MTLDevice> dev = facade::device();
        ASSERT_NE(dev, nil);
        static TegraGM20BAbiGrPreempt seen = {};
        static unsigned calls = 0;
        seen = {};
        calls = 0;
        uint32_t stored = TegraGM20BGrPreemptWfi;
        kernel_model::arriving.store(
            [](uint32_t method, const void *input) -> int {
                if (method == TegraGM20BMethodGrPreempt) {
                    seen = *static_cast<const TegraGM20BAbiGrPreempt *>(input);
                    ++calls;
                }
                return KERN_SUCCESS;
            },
            std::memory_order_release);
        kernel_model::model.otherMethod = [&](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodGrPreempt)
                return kIOReturnUnsupported;
            auto *reply = static_cast<TegraGM20BAbiGrPreemptReply *>(output);
            reply->old_mode = stored;
            stored = seen.mode;
            reply->readback = stored;
            return KERN_SUCCESS;
        };
        const auto toggle = [&](BOOL on) -> NSString * {
            return ((NSString * (*)(id, SEL, NSNumber *))
                        objc_msgSend)(dev, NSSelectorFromString(@"switchMetalProbeComputeCta:"), @(on));
        };
        NSString *onReply = toggle(YES);
        EXPECT_STREQ(onReply.UTF8String, "old=0 readback=1");
        EXPECT_EQ(calls, 1u);
        EXPECT_GT(seen.channel, 0u);
        EXPECT_EQ(seen.mode, uint32_t(TegraGM20BGrPreemptCta));
        EXPECT_EQ(seen.reserved[0], 0u);
        EXPECT_EQ(seen.reserved[1], 0u);

        NSString *offReply = toggle(NO);
        EXPECT_STREQ(offReply.UTF8String, "old=1 readback=0");
        EXPECT_EQ(calls, 2u);
        EXPECT_EQ(seen.mode, uint32_t(TegraGM20BGrPreemptWfi));

        kernel_model::model.otherMethod = nullptr;
        NSString *errReply = toggle(YES);
        EXPECT_TRUE([errReply hasPrefix:@"error=0x"]);
        kernel_model::arriving.store(nullptr, std::memory_order_release);
    }
}

// A host test binary holds no entitlements: both routes to raw 3D-class methods refuse it before taking one.
TEST(FacadeProbe, RawMethodsNeedTheProbesEntitlement)
{
    @autoreleasepool {
        id<MTLTexture> written = target();
        ASSERT_NE(written, nil);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        NSString *const entitlement = @(SWITCH_GPU_PROBES_ENTITLEMENT);
        NSString *sent = engine(
            cb, @{
                @"kind" : @"method",
                @"method" : @0x1234,
                @"value" : @1,
                @"written" : written,
                @"unbracketed" : @1,
            });
        EXPECT_TRUE([sent containsString:entitlement]) << sent.UTF8String;
        [blit endEncoding];
        const uint32_t pair[2] = {0x1234, 1};
        NSString *armed = ((NSString * (*)(id, SEL, NSData *, NSData *))
                               objc_msgSend)(facade::device(), NSSelectorFromString(@"switchMetalProbeBefore:after:"),
                                             [NSData dataWithBytes:pair length:sizeof(pair)], nil);
        EXPECT_TRUE([armed containsString:entitlement]) << armed.UTF8String;
    }
}

}  // namespace
