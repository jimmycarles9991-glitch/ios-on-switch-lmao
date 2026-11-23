// SPDX-License-Identifier: MIT
// A command buffer owns its pass bank until it returns its recording, and the bank's copies name its textures no longer;
// pressure and idle release reclaim only the banks of the recordings kept.
#import <Metal/Metal.h>
#include "../TegraGM20BMetalPassCache.h"
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kShapes = 8, kLookups = 4096;

class GM20BPassCacheRace : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gm20bPoolCommandBuffers();
    }

    void SetUp() override
    {
        gm20bPassTrim();
    }
};

MTLRenderPassDescriptor *colourPass(SwitchMetalTexture *target, MTLLoadAction load = MTLLoadActionClear)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = (id<MTLTexture>)target;
    d.colorAttachments[0].loadAction = load;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return d;
}

void keepPass(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *d, const std::vector<uint8_t> &key, uint64_t hash)
{
    SwitchMetalPass facts{};
    facts.color[0].texture = d.colorAttachments[0].texture;
    gm20bPassKeep(cb, key, hash, [d copy], facts, [SwitchMetalRenderEncoder new]);
}

TEST_F(GM20BPassCacheRace, ATrimKeepsTheBorrowedEntryValidUntilItsRecordingReturns)
{
    std::mutex lock;
    std::condition_variable changed;
    bool stop = false, borrowed = false, trimmedBorrow = false;
    int looked = 0, hits = 0, read = 0;
    std::thread encoder([&] {
        @autoreleasepool {
            std::vector<MTLRenderPassDescriptor *> passes;
            for (int i = 0; i < kShapes; i++)
                passes.push_back(colourPass([SwitchMetalTexture new], i % 2 ? MTLLoadActionLoad : MTLLoadActionClear));
            std::vector<uint8_t> key;
            uint64_t hash = 0;
            for (int base = 0; base < kLookups; base += 8 * kShapes) {
                SwitchMetalCommandBuffer *cb = [SwitchMetalCommandBuffer new];
                gm20bTakeRecording(cb);
                for (int n = base; n < base + 8 * kShapes; n++) {
                    MTLRenderPassDescriptor *d = passes[size_t(n % kShapes)];
                    if (!gm20bPassKey(d, key, hash)) {
                        ADD_FAILURE() << "no key for Metal's own descriptor";
                        break;
                    }
                    if (const SwitchMetalPassEntry *kept = gm20bPassFind(cb, key, hash)) {
                        if (!borrowed) {
                            std::unique_lock<std::mutex> held(lock);
                            borrowed = true;
                            changed.notify_one();
                            changed.wait(held, [&] {
                                return trimmedBorrow;
                            });
                        }
                        gm20bPassBind(cb, kept->pass);
                        read += kept->pass.colorAttachments[0].texture == d.colorAttachments[0].texture;
                        hits++;
                    } else {
                        keepPass(cb, d, key, hash);
                    }
                    if ((n + 1) % (2 * kShapes) == 0) {
                        std::lock_guard<std::mutex> held(lock);
                        looked = n + 1;
                        changed.notify_one();
                    }
                }
                gm20bRecordingDone(cb);
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
        gm20bPassTrim();
        trims++;
        held.lock();
        trimmedBorrow = true;
        changed.notify_one();
    }
    encoder.join();
    EXPECT_EQ(read, hits) << "every pass found survived the trim with its texture";
    EXPECT_GT(hits, 0);
    EXPECT_GT(trims, 0u);
    gm20bPassTrim();
    EXPECT_EQ(gm20bPassHeld().passes, 0u);
}

// Recordings pass between two threads through the pool alone: each names the pass's texture in the kept copy it finds, as a
// replay asks, and lets it go as it returns, with no other ordering between the threads
TEST_F(GM20BPassCacheRace, BanksPassedBetweenThreadsNameTheirTexturesOnlyWhileTaken)
{
    constexpr int kRounds = 512;
    __weak SwitchMetalTexture *gone;
    std::atomic<int> hits{0}, read{0};
    @autoreleasepool {
        SwitchMetalTexture *target = [SwitchMetalTexture new];
        gone = target;
        const auto run = [&](MTLRenderPassDescriptor *d) {
            std::vector<uint8_t> key;
            uint64_t hash = 0;
            for (int n = 0; n < kRounds; n++) {
                @autoreleasepool {
                    SwitchMetalCommandBuffer *cb = [SwitchMetalCommandBuffer new];
                    gm20bTakeRecording(cb);
                    EXPECT_TRUE(gm20bPassKey(d, key, hash));
                    if (const SwitchMetalPassEntry *kept = gm20bPassFind(cb, key, hash)) {
                        gm20bPassBind(cb, kept->pass);
                        read += kept->pass.colorAttachments[0].texture == (id<MTLTexture>)target;
                        hits++;
                    } else {
                        keepPass(cb, d, key, hash);
                    }
                    gm20bRecordingDone(cb);
                }
            }
        };
        MTLRenderPassDescriptor *mine = colourPass(target), *theirs = colourPass(target);
        std::thread other(run, theirs);
        run(mine);
        other.join();
    }
    EXPECT_GT(hits.load(), 0);
    EXPECT_EQ(read.load(), hits.load()) << "a found copy named no texture or another";
    EXPECT_EQ(gone, nil) << "a returned recording's copy outlived its command buffer with the texture";
}

}  // namespace
