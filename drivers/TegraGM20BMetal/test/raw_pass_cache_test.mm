// SPDX-License-Identifier: MIT
// The pass cache's key (RawMetalPassCache.mm) over Metal's own descriptor objects: passes that differ only in their
// depth attachment's resolve filter are two keys and two kept entries, so a kept pass never hands a later one its depth
// resolve facts (RMPass::depthFilter). The textures are the host's Metal device's: the key reads only their addresses.
#import <Metal/Metal.h>
#include "../raw/runtime/RawMetalPassCache.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

namespace {

id<MTLTexture> depthTexture(id<MTLDevice> device, NSUInteger samples)
{
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    t.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    t.sampleCount = samples;
    t.storageMode = MTLStorageModePrivate;
    t.usage = MTLTextureUsageRenderTarget;
    return [device newTextureWithDescriptor:t];
}

MTLRenderPassDescriptor *resolvingPass(id<MTLTexture> depth, id<MTLTexture> resolve,
                                       MTLMultisampleDepthResolveFilter filter)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.depthAttachment.texture = depth;
    d.depthAttachment.loadAction = MTLLoadActionClear;
    d.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
    d.depthAttachment.resolveTexture = resolve;
    d.depthAttachment.depthResolveFilter = filter;
    return d;
}

struct Key {
    std::vector<uint8_t> bytes;
    uint64_t hash = 0;
};

class RawPassCache : public testing::Test {
protected:
    id<MTLTexture> depth, resolve;

    void SetUp() override
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
            GTEST_SKIP() << "no Metal device on this host";
        depth = depthTexture(device, 4);
        resolve = depthTexture(device, 1);
        ASSERT_NE(depth, nil);
        ASSERT_NE(resolve, nil);
    }

    Key keyOf(MTLMultisampleDepthResolveFilter filter)
    {
        Key k;
        EXPECT_TRUE(rmPassKey(resolvingPass(depth, resolve, filter), k.bytes, k.hash));
        return k;
    }
};

TEST_F(RawPassCache, TheSamePassIsTheSameKey)
{
    const Key a = keyOf(MTLMultisampleDepthResolveFilterMin), b = keyOf(MTLMultisampleDepthResolveFilterMin);
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(a.hash, b.hash);
}

TEST_F(RawPassCache, ADepthResolveFilterIsPartOfTheKey)
{
    const Key lowest = keyOf(MTLMultisampleDepthResolveFilterMin), highest = keyOf(MTLMultisampleDepthResolveFilterMax);
    ASSERT_FALSE(lowest.bytes.empty());
    EXPECT_NE(lowest.bytes, highest.bytes);
}

TEST_F(RawPassCache, PassesOfTwoDepthResolveFiltersAreTwoEntries)
{
    const MTLMultisampleDepthResolveFilter filters[2] = {MTLMultisampleDepthResolveFilterMin,
                                                         MTLMultisampleDepthResolveFilterMax};
    Key keys[2];
    RMRenderEncoder *e = [RMRenderEncoder new];
    for (int i = 0; i < 2; i++) {
        keys[i] = keyOf(filters[i]);
        RMPass facts{};
        facts.depthResolve = resolve;
        facts.depthFilter = uint32_t(filters[i]);
        rmPassKeep(keys[i].bytes, keys[i].hash, resolvingPass(depth, resolve, filters[i]), facts, e);
    }
    const RMPassEntry *kept[2] = {rmPassFind(keys[0].bytes, keys[0].hash), rmPassFind(keys[1].bytes, keys[1].hash)};
    ASSERT_NE(kept[0], nullptr);
    ASSERT_NE(kept[1], nullptr);
    EXPECT_NE(kept[0], kept[1]);
    for (int i = 0; i < 2; i++)
        EXPECT_EQ(kept[i]->facts.depthFilter, uint32_t(filters[i])) << i;
}

}  // namespace
