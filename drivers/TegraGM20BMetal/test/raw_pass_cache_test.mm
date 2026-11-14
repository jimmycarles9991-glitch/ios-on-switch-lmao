// SPDX-License-Identifier: MIT
// The pass cache's key (RawMetalPassCache.mm) over Metal's own descriptor objects: passes that differ only in their
// depth attachment's resolve filter are two keys and two kept entries, so a kept pass never hands a later one its depth
// resolve facts (RMPass::depthFilter); a compositor frame's passes (boot 21: 29-40 render targets a command buffer) are
// all kept from one frame to the next. The textures are the host's Metal device's: the key reads only their addresses.
#import <Metal/Metal.h>
#include "../raw/runtime/RawMetalPassCache.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <chrono>
#include <future>
#include <thread>
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
    const RMPassEntry *kept[2] = {};
    for (int i = 0; i < 2; i++) {
        kept[i] = rmPassFind(keys[i].bytes, keys[i].hash);
        ASSERT_NE(kept[i], nullptr);
        EXPECT_EQ(kept[i]->facts.depthFilter, uint32_t(filters[i])) << i;
        rmPassDone();
    }
    EXPECT_NE(kept[0], kept[1]);
}

TEST_F(RawPassCache, AFrameOfFortyPassesIsKeptForTheNext)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    t.storageMode = MTLStorageModePrivate;
    t.usage = MTLTextureUsageRenderTarget;
    std::vector<MTLRenderPassDescriptor *> passes;
    for (int i = 0; i < 20; i++) {
        id<MTLTexture> target = [device newTextureWithDescriptor:t];
        for (MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad}) {
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = target;
            d.colorAttachments[0].loadAction = load;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            passes.push_back(d);
        }
    }
    RMRenderEncoder *e = [RMRenderEncoder new];
    std::vector<Key> keys(passes.size());
    for (size_t i = 0; i < passes.size(); i++) {
        ASSERT_TRUE(rmPassKey(passes[i], keys[i].bytes, keys[i].hash));
        if (rmPassFind(keys[i].bytes, keys[i].hash))
            rmPassDone();
        else
            rmPassKeep(keys[i].bytes, keys[i].hash, [passes[i] copy], RMPass {}, e);
    }
    size_t found = 0;
    for (const Key &k : keys)
        if (rmPassFind(k.bytes, k.hash)) {
            ++found;
            rmPassDone();
        }
    EXPECT_EQ(found, passes.size());
}

// After an animation the render server's encoding thread may encode nothing for a long while, and its kept passes'
// copies hold the textures of its last frame: memory pressure's trim (gRawRelease, on its own queue) lets them go with
// no lookup on that thread.
TEST_F(RawPassCache, TrimLetsAnIdleThreadsPassesGo)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    __weak id<MTLTexture> targets[3];
    std::promise<void> kept, done;
    std::thread encoder([&] {
        @autoreleasepool {
            MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:64
                                                                                        height:64
                                                                                     mipmapped:NO];
            t.storageMode = MTLStorageModePrivate;
            t.usage = MTLTextureUsageRenderTarget;
            RMRenderEncoder *e = [RMRenderEncoder new];
            for (int i = 0; i < 3; i++) {
                id<MTLTexture> target = [device newTextureWithDescriptor:t];
                targets[i] = target;
                MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = target;
                d.colorAttachments[0].loadAction = MTLLoadActionClear;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                Key k;
                ASSERT_TRUE(rmPassKey(d, k.bytes, k.hash));
                RMPass facts{};
                facts.color[0].texture = target;
                rmPassKeep(k.bytes, k.hash, [d copy], facts, e);
            }
        }
        kept.set_value();
        done.get_future().wait();
    });
    kept.get_future().wait();
    for (int i = 0; i < 3; i++)
        EXPECT_NE(targets[i], nil) << "kept by the encoding thread's pass copy " << i;
    const RMPassTrim trimmed = rmPassTrim();
    EXPECT_GE(trimmed.passes, 3u);
    EXPECT_GE(trimmed.textures, 3u);
    for (int i = 0; i < 3; i++)
        EXPECT_EQ(targets[i], nil) << "texture " << i << " still held after trim";
    done.set_value();
    encoder.join();
}

}  // namespace

// A trim that meets a thread inside a pass it found takes nothing of that thread's: the thread is reading the copy. The
// thread lets its copies go at its next lookup instead.
TEST_F(RawPassCache, ATrimLeavesAPassItsThreadIsReadingForItsNextLookup)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    __weak id<MTLTexture> weakTarget;
    std::promise<void> reading, trimmed, looked;
    std::promise<void> done;
    std::thread encoder([&] {
        Key k;
        @autoreleasepool {
            MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:64
                                                                                        height:64
                                                                                     mipmapped:NO];
            t.storageMode = MTLStorageModePrivate;
            t.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [device newTextureWithDescriptor:t];
            weakTarget = target;
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = target;
            d.colorAttachments[0].loadAction = MTLLoadActionClear;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            ASSERT_TRUE(rmPassKey(d, k.bytes, k.hash));
            RMPass facts{};
            facts.color[0].texture = target;
            rmPassKeep(k.bytes, k.hash, [d copy], facts, [RMRenderEncoder new]);
        }
        const RMPassEntry *kept = rmPassFind(k.bytes, k.hash);
        ASSERT_NE(kept, nullptr);
        reading.set_value();
        trimmed.get_future().wait();
        EXPECT_NE(kept->pass, nil) << "the copy this thread reads went under it";
        rmPassDone();
        EXPECT_EQ(rmPassFind(k.bytes, k.hash), nullptr) << "the trim's ask is met at the next lookup";
        looked.set_value();
        done.get_future().wait();
    });
    reading.get_future().wait();
    auto trim = std::async(std::launch::async, [] {
        return rmPassTrim();
    });
    const bool returned = trim.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(returned) << "the trim waited for the thread inside its pass";
    EXPECT_NE(weakTarget, nil);
    trimmed.set_value();
    trim.wait();
    looked.get_future().wait();
    EXPECT_EQ(weakTarget, nil) << "the thread let its copies go at its next lookup";
    done.set_value();
    encoder.join();
}
