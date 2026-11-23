// SPDX-License-Identifier: MIT
// The pass cache's key (TegraGM20BMetalPassCache.mm) over Metal's own descriptor objects: passes that differ only in their
// depth attachment's resolve filter are two keys and two kept entries, so a kept pass never hands a later one its depth
// resolve facts (SwitchMetalPass::depthFilter); a compositor frame's passes (boot 21: 29-40 render targets a command buffer) are
// all kept from one frame to the next. The attachments are the facade's textures with no device behind them: the key
// reads only their identities, and a returned recording's copies keep none of them alive.
#import <Metal/Metal.h>
#include "../TegraGM20BMetalPassCache.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace {

MTLRenderPassDescriptor *resolvingPass(SwitchMetalTexture *depth, SwitchMetalTexture *resolve,
                                       MTLMultisampleDepthResolveFilter filter)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.depthAttachment.texture = (id<MTLTexture>)depth;
    d.depthAttachment.loadAction = MTLLoadActionClear;
    d.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
    d.depthAttachment.resolveTexture = (id<MTLTexture>)resolve;
    d.depthAttachment.depthResolveFilter = filter;
    return d;
}

MTLRenderPassDescriptor *colourPass(SwitchMetalTexture *target, MTLLoadAction load = MTLLoadActionClear)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = (id<MTLTexture>)target;
    d.colorAttachments[0].loadAction = load;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    return d;
}

struct Key {
    std::vector<uint8_t> bytes;
    uint64_t hash = 0;
};

SwitchMetalCommandBuffer *takenCommandBuffer()
{
    SwitchMetalCommandBuffer *cb = [SwitchMetalCommandBuffer new];
    gm20bTakeRecording(cb);
    return cb;
}

// A copy of `d` kept in `cb`'s bank, as a pass opened from it keeps one; the copy itself
MTLRenderPassDescriptor *keepPass(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *d)
{
    Key k;
    EXPECT_TRUE(gm20bPassKey(d, k.bytes, k.hash));
    SwitchMetalPass facts{};
    facts.color[0].texture = d.colorAttachments[0].texture;
    MTLRenderPassDescriptor *copy = [d copy];
    gm20bPassKeep(cb, k.bytes, k.hash, copy, facts, [SwitchMetalRenderEncoder new]);
    return copy;
}

const SwitchMetalPassEntry *findPass(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *d)
{
    Key k;
    return gm20bPassKey(d, k.bytes, k.hash) ? gm20bPassFind(cb, k.bytes, k.hash) : nullptr;
}

class GM20BPassCache : public testing::Test {
protected:
    SwitchMetalTexture *depth, *resolve;
    SwitchMetalCommandBuffer *cb;

    static void SetUpTestSuite()
    {
        gm20bPoolCommandBuffers();
    }

    void SetUp() override
    {
        gm20bPassTrim();
        cb = takenCommandBuffer();
        depth = [SwitchMetalTexture new];
        resolve = [SwitchMetalTexture new];
    }

    void TearDown() override
    {
        if (cb)
            gm20bRecordingDone(cb);
    }

    Key keyOf(MTLMultisampleDepthResolveFilter filter)
    {
        Key k;
        EXPECT_TRUE(gm20bPassKey(resolvingPass(depth, resolve, filter), k.bytes, k.hash));
        return k;
    }
};

TEST_F(GM20BPassCache, TheSamePassIsTheSameKey)
{
    const Key a = keyOf(MTLMultisampleDepthResolveFilterMin), b = keyOf(MTLMultisampleDepthResolveFilterMin);
    EXPECT_EQ(a.bytes, b.bytes);
    EXPECT_EQ(a.hash, b.hash);
}

TEST_F(GM20BPassCache, ADepthResolveFilterIsPartOfTheKey)
{
    const Key lowest = keyOf(MTLMultisampleDepthResolveFilterMin), highest = keyOf(MTLMultisampleDepthResolveFilterMax);
    ASSERT_FALSE(lowest.bytes.empty());
    EXPECT_NE(lowest.bytes, highest.bytes);
}

TEST_F(GM20BPassCache, PassesOfTwoDepthResolveFiltersAreTwoEntries)
{
    const MTLMultisampleDepthResolveFilter filters[2] = {MTLMultisampleDepthResolveFilterMin,
                                                         MTLMultisampleDepthResolveFilterMax};
    Key keys[2];
    SwitchMetalRenderEncoder *e = [SwitchMetalRenderEncoder new];
    for (int i = 0; i < 2; i++) {
        keys[i] = keyOf(filters[i]);
        SwitchMetalPass facts{};
        facts.zeta[0].texture = depth;
        facts.depthResolve = resolve;
        facts.depthFilter = uint32_t(filters[i]);
        gm20bPassKeep(cb, keys[i].bytes, keys[i].hash, [resolvingPass(depth, resolve, filters[i]) copy], facts, e);
    }
    const SwitchMetalPassEntry *kept[2] = {};
    for (int i = 0; i < 2; i++) {
        kept[i] = gm20bPassFind(cb, keys[i].bytes, keys[i].hash);
        ASSERT_NE(kept[i], nullptr);
        EXPECT_EQ(kept[i]->facts.depthFilter, uint32_t(filters[i])) << i;
    }
    EXPECT_NE(kept[0], kept[1]);
}

TEST_F(GM20BPassCache, AFrameOfFortyPassesIsKeptForTheNext)
{
    std::vector<MTLRenderPassDescriptor *> passes;
    for (int i = 0; i < 20; i++) {
        SwitchMetalTexture *target = [SwitchMetalTexture new];
        for (MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad})
            passes.push_back(colourPass(target, load));
    }
    for (MTLRenderPassDescriptor *d : passes)
        if (!findPass(cb, d))
            keepPass(cb, d);
    gm20bRecordingDone(cb);
    cb = takenCommandBuffer();
    size_t found = 0;
    for (MTLRenderPassDescriptor *d : passes)
        found += findPass(cb, d) != nullptr;
    EXPECT_EQ(found, passes.size());
}

TEST_F(GM20BPassCache, ACommandBufferLeavesNoTextureItUsedAliveOnceItReturns)
{
    __weak SwitchMetalTexture *gone;
    @autoreleasepool {
        SwitchMetalTexture *target = [SwitchMetalTexture new];
        gone = target;
        SwitchMetalCommandBuffer *b = takenCommandBuffer();
        keepPass(b, colourPass(target));
        gm20bRecordingDone(b);
    }
    EXPECT_EQ(gone, nil) << "the returned recording's kept pass still names its texture";
    EXPECT_GE(gm20bPassHeld().passes, 1u) << "the pass itself is kept";
    EXPECT_EQ(gm20bPassHeld().textures, 0u);
}

TEST_F(GM20BPassCache, ABurstOfTwentyFourLeavesNoTextureAliveOnceItReturns)
{
    constexpr int kBurst = 24;
    __weak SwitchMetalTexture *gone[kBurst];
    @autoreleasepool {
        std::vector<SwitchMetalCommandBuffer *> burst;
        for (int i = 0; i < kBurst; i++) {
            SwitchMetalTexture *target = [SwitchMetalTexture new];
            gone[i] = target;
            burst.push_back(takenCommandBuffer());
            keepPass(burst.back(), colourPass(target));
        }
        for (SwitchMetalCommandBuffer *b : burst)
            gm20bRecordingDone(b);
    }
    for (int i = 0; i < kBurst; i++)
        EXPECT_EQ(gone[i], nil) << "texture " << i << " outlived its command buffer";
}

TEST_F(GM20BPassCache, ACommandBufferOnTheRawPathNeverNamesTexturesInAFoundCopy)
{
    SwitchMetalTexture *target = [SwitchMetalTexture new];
    MTLRenderPassDescriptor *d = colourPass(target);
    SwitchMetalCommandBuffer *first = takenCommandBuffer();
    MTLRenderPassDescriptor *copy = keepPass(first, d);
    gm20bRecordingDone(first);
    EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "the returned copy still names the texture";
    SwitchMetalCommandBuffer *next = takenCommandBuffer();
    const SwitchMetalPassEntry *kept = findPass(next, d);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->pass, copy) << "the pass was copied again";
    EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "a hit named the texture with no replay asking";
    gm20bRecordingDone(next);
    EXPECT_EQ(copy.colorAttachments[0].texture, nil);
}

TEST_F(GM20BPassCache, AReplayFindsTheTexturesInTheCopyUntilItsCommandBufferReturns)
{
    __weak SwitchMetalTexture *gone;
    MTLRenderPassDescriptor *copy = nil;
    @autoreleasepool {
        SwitchMetalTexture *target = [SwitchMetalTexture new];
        gone = target;
        MTLRenderPassDescriptor *d = colourPass(target);
        SwitchMetalCommandBuffer *first = takenCommandBuffer();
        keepPass(first, d);
        gm20bRecordingDone(first);
        SwitchMetalCommandBuffer *next = takenCommandBuffer();
        const SwitchMetalPassEntry *kept = findPass(next, d);
        ASSERT_NE(kept, nullptr);
        copy = kept->pass;
        gm20bPassBind(next, copy);
        EXPECT_EQ(copy.colorAttachments[0].texture, (id<MTLTexture>)target) << "the replay's copy names no texture";
        gm20bRecordingDone(next);
        EXPECT_EQ(copy.colorAttachments[0].texture, nil) << "the replayed copy still names the texture";
    }
    EXPECT_EQ(gone, nil);
}

TEST_F(GM20BPassCache, ANewTextureWhereADeadOneWasFindsNothingOfIt)
{
    SwitchMetalTexture *target = [SwitchMetalTexture new];
    MTLRenderPassDescriptor *d = colourPass(target);
    SwitchMetalCommandBuffer *first = takenCommandBuffer();
    keepPass(first, d);
    gm20bRecordingDone(first);
    SwitchMetalCommandBuffer *next = takenCommandBuffer();
    ASSERT_NE(findPass(next, d), nullptr);
    target->_identity |= uint64_t(1) << 63;  // another texture at the same address
    EXPECT_EQ(findPass(next, d), nullptr) << "a kept pass of the dead texture's was found by its address";
    gm20bRecordingDone(next);
}

TEST_F(GM20BPassCache, ABatchOutAtOnceFindsItsPassesInTheNextBatch)
{
    constexpr int kBatch = 24;
    MTLRenderPassDescriptor *frame = colourPass([SwitchMetalTexture new]);
    int found = 0;
    for (int round = 0; round < 2; round++) {
        std::vector<SwitchMetalCommandBuffer *> batch;
        for (int i = 0; i < kBatch; i++) {
            batch.push_back(takenCommandBuffer());
            if (findPass(batch.back(), frame))
                found += round;
            else
                keepPass(batch.back(), frame);
        }
        for (SwitchMetalCommandBuffer *b : batch)
            gm20bRecordingDone(b);
    }
    EXPECT_EQ(found, kBatch) << "a batch's kept passes were let go as they returned";
}

TEST_F(GM20BPassCache, ATrimLetsAReturnedRecordingsPassesGo)
{
    SwitchMetalCommandBuffer *b = takenCommandBuffer();
    for (int i = 0; i < 3; i++)
        keepPass(b, colourPass([SwitchMetalTexture new]));
    gm20bRecordingDone(b);
    EXPECT_GE(gm20bPassTrim().passes, 3u);
    EXPECT_EQ(gm20bPassHeld().passes, 0u);
}

TEST_F(GM20BPassCache, CountingAndTrimmingLeaveTheNextCommandBufferWhatReturnedLast)
{
    SwitchMetalCommandBuffer *first = takenCommandBuffer(), *last = takenCommandBuffer();
    first->_shadow.reserve(4096);
    last->_shadow.reserve(first->_shadow.capacity() + 4096);
    const void *lastBank = last->_passCache.get();
    const size_t lastShadow = last->_shadow.capacity();
    gm20bRecordingDone(first);
    gm20bRecordingDone(last);
    gm20bPassHeld();
    gm20bPassTrim();
    SwitchMetalCommandBuffer *next = takenCommandBuffer();
    EXPECT_EQ(static_cast<const void *>(next->_passCache.get()), lastBank) << "the kept recordings came back reversed";
    EXPECT_EQ(next->_shadow.capacity(), lastShadow);
    gm20bRecordingDone(next);
}

TEST_F(GM20BPassCache, ATrimLeavesAPassItsRecordingOwnsUntilItReturns)
{
    __weak SwitchMetalTexture *weakTarget;
    std::promise<void> reading, trimmed, looked;
    std::promise<void> done;
    std::thread encoder([&] {
        SwitchMetalCommandBuffer *b = takenCommandBuffer();
        MTLRenderPassDescriptor *d = nil;
        @autoreleasepool {
            SwitchMetalTexture *target = [SwitchMetalTexture new];
            weakTarget = target;
            d = colourPass(target);
            keepPass(b, d);
        }
        const SwitchMetalPassEntry *kept = findPass(b, d);
        ASSERT_NE(kept, nullptr);
        d = nil;
        reading.set_value();
        trimmed.get_future().wait();
        EXPECT_NE(kept->pass, nil) << "the copy this thread reads went under it";
        gm20bRecordingDone(b);
        looked.set_value();
        done.get_future().wait();
    });
    reading.get_future().wait();
    auto trim = std::async(std::launch::async, [] {
        return gm20bPassTrim();
    });
    const bool returned = trim.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(returned) << "the trim waited for the thread inside its pass";
    EXPECT_NE(weakTarget, nil);
    trimmed.set_value();
    trim.wait();
    looked.get_future().wait();
    EXPECT_EQ(weakTarget, nil) << "the returned recording's copy still names its texture";
    done.set_value();
    encoder.join();
}

}  // namespace
