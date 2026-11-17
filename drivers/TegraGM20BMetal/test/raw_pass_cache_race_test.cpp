// SPDX-License-Identifier: MIT
// A recording owns its pass-cache bank while idle/pressure release reclaims only banks the recording pool owns.
#import <Metal/Metal.h>
#include "../raw/runtime/RawMetalPassCache.h"
#include <gtest/gtest.h>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

struct SwitchPassReturnGate {
    std::promise<void> entered, released;
};

@interface SwitchPassReturnObject : NSObject {
@public
    SwitchPassReturnGate *_gate;
}
@end

@implementation SwitchPassReturnObject

- (void)dealloc
{
    _gate->entered.set_value();
    _gate->released.get_future().wait();
}

@end

namespace {

constexpr int kShapes = 8, kLookups = 4096;

class RawPassCacheRace : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        rmPoolCommandBuffers();
    }

    void SetUp() override
    {
        rmPassTrim();
    }
};

MTLTextureDescriptor *targetDescriptor()
{
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    t.storageMode = MTLStorageModePrivate;
    t.usage = MTLTextureUsageRenderTarget;
    return t;
}

TEST_F(RawPassCacheRace, ATrimKeepsTheBorrowedEntryValidUntilItsRecordingReturns)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        GTEST_SKIP() << "no Metal device on this host";
    std::mutex lock;
    std::condition_variable changed;
    bool stop = false, borrowed = false, trimmedBorrow = false;
    int looked = 0, hits = 0, read = 0;
    std::thread encoder([&] {
        @autoreleasepool {
            RMRenderEncoder *e = [RMRenderEncoder new];
            std::vector<MTLRenderPassDescriptor *> passes;
            for (int i = 0; i < kShapes; i++) {
                MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = [device newTextureWithDescriptor:targetDescriptor()];
                d.colorAttachments[0].loadAction = i % 2 ? MTLLoadActionLoad : MTLLoadActionClear;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                passes.push_back(d);
            }
            std::vector<uint8_t> key;
            uint64_t hash = 0;
            for (int base = 0; base < kLookups; base += 8 * kShapes) {
                RMCommandBuffer *cb = [RMCommandBuffer new];
                rmTakeRecording(cb);
                for (int n = base; n < base + 8 * kShapes; n++) {
                    MTLRenderPassDescriptor *d = passes[size_t(n % kShapes)];
                    if (!rmPassKey(d, key, hash)) {
                        ADD_FAILURE() << "no key for Metal's own descriptor";
                        break;
                    }
                    if (const RMPassEntry *kept = rmPassFind(cb, key, hash)) {
                        if (!borrowed) {
                            std::unique_lock<std::mutex> held(lock);
                            borrowed = true;
                            changed.notify_one();
                            changed.wait(held, [&] {
                                return trimmedBorrow;
                            });
                        }
                        read += kept->pass.colorAttachments[0].texture == d.colorAttachments[0].texture;
                        hits++;
                    } else {
                        RMPass facts{};
                        facts.color[0].texture = d.colorAttachments[0].texture;
                        rmPassKeep(cb, key, hash, [d copy], facts, e);
                    }
                    if ((n + 1) % (2 * kShapes) == 0) {
                        std::lock_guard<std::mutex> held(lock);
                        looked = n + 1;
                        changed.notify_one();
                    }
                }
                rmRecordingDone(cb);
                if (!base)
                    EXPECT_EQ(rmPassHeld().passes, 0u) << "the borrowed bank retires pressure's copies on return";
            }
        }
        std::lock_guard<std::mutex> held(lock);
        stop = true;
        changed.notify_one();
    });
    size_t trims = 0;
    for (int seen = 0;;) {
        std::unique_lock<std::mutex> held(lock);
        changed.wait(held, [&] {
            return stop || looked != seen || (borrowed && !trimmedBorrow);
        });
        if (stop)
            break;
        seen = looked;
        held.unlock();
        rmPassTrim();
        trims++;
        held.lock();
        trimmedBorrow = true;
        changed.notify_one();
    }
    encoder.join();
    EXPECT_EQ(read, hits) << "every pass found survived the trim with its texture";
    EXPECT_GT(hits, 0);
    EXPECT_GT(trims, 0u);
    rmPassTrim();
    EXPECT_EQ(rmPassHeld().passes, 0u);
    printf("lookups %d hits %d trims %zu\n", kLookups, hits, trims);
}

TEST_F(RawPassCacheRace, AnIdleBankReleasesItsTexturesWithoutAnotherRecording)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        GTEST_SKIP() << "no Metal device on this host";
    __weak id<MTLTexture> texture;
    @autoreleasepool {
        RMCommandBuffer *cb = [RMCommandBuffer new];
        rmTakeRecording(cb);
        RMRenderEncoder *e = [RMRenderEncoder new];
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = [device newTextureWithDescriptor:targetDescriptor()];
        texture = d.colorAttachments[0].texture;
        std::vector<uint8_t> key;
        uint64_t hash = 0;
        ASSERT_TRUE(rmPassKey(d, key, hash));
        RMPass facts{};
        facts.color[0].texture = texture;
        rmPassKeep(cb, key, hash, [d copy], facts, e);
        rmRecordingDone(cb);
    }
    ASSERT_NE(texture, nil);
    EXPECT_GE(rmPassHeld().passes, 1u);
    EXPECT_GE(rmPassTrim().passes, 1u);
    EXPECT_EQ(texture, nil);
    EXPECT_EQ(rmPassHeld().passes, 0u);
}

TEST_F(RawPassCacheRace, ATrimBeforeARacingReturnPublishesStillReleasesItsTextures)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        GTEST_SKIP() << "no Metal device on this host";
    SwitchPassReturnGate gate;
    __weak id<MTLTexture> texture;
    std::thread returning([&] {
        @autoreleasepool {
            RMCommandBuffer *cb = [RMCommandBuffer new];
            rmTakeRecording(cb);
            @autoreleasepool {
                MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
                d.colorAttachments[0].texture = [device newTextureWithDescriptor:targetDescriptor()];
                texture = d.colorAttachments[0].texture;
                std::vector<uint8_t> key;
                uint64_t hash = 0;
                EXPECT_TRUE(rmPassKey(d, key, hash));
                RMPass facts{};
                facts.color[0].texture = texture;
                rmPassKeep(cb, key, hash, [d copy], facts, [RMRenderEncoder new]);
            }
            SwitchPassReturnObject *object = [SwitchPassReturnObject new];
            object->_gate = &gate;
            cb->_owners.push_back(object);
            object = nil;
            rmRecordingDone(cb);
        }
    });
    gate.entered.get_future().wait();
    EXPECT_NE(texture, nil);
    rmPassTrim();
    EXPECT_NE(texture, nil) << "the return still owns the unpublished bank";
    gate.released.set_value();
    returning.join();
    EXPECT_EQ(texture, nil) << "the raced publication escaped pressure's idle-bank drain";
    EXPECT_EQ(rmPassHeld().passes, 0u);
}

}  // namespace
