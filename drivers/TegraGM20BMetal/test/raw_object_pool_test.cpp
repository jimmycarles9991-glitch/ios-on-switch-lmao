// SPDX-License-Identifier: MIT
// Objective-C++ with ARC, as the facade's encoders are (raw_object_pool.h)
#import <Foundation/Foundation.h>
#include "../raw/runtime/raw_object_pool.h"
#include <gtest/gtest.h>
#include <objc/runtime.h>
#include <atomic>
#include <thread>
#include <vector>
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif

namespace {

std::atomic<int> gDropped{0}, gDestroyed{0};

struct Tracked {
    std::vector<int> values;

    ~Tracked()
    {
        if (!values.empty())
            gDestroyed++;
    }
};

}  // namespace

@interface PoolProbe : NSObject {
@public
    Tracked _tracked;
    id _strong;
    __weak id _weak;
    uint64_t _word;
}
@end

@implementation PoolProbe
@end

@interface PoolProbeChild : PoolProbe {
@public
    uint64_t _more;
}
@end

@implementation PoolProbeChild
@end

// A command buffer's shape (RawMetalCommandBuffer.mm): its pool's dropped gives its recording back
@interface PoolBufferProbe : NSObject {
@public
    std::vector<int> _recording;
}
@end

@implementation PoolBufferProbe
@end

namespace {

std::atomic<int> gRecordingsGiven{0};

void dropped(void *)
{
    gDropped++;
}

void recordingGiven(void *object)
{
    PoolBufferProbe *__unsafe_unretained b = (__bridge PoolBufferProbe *)object;
    if (!b->_recording.empty())
        gRecordingsGiven++;
}

void pooled()
{
    static const bool once = rawPoolInstances([PoolProbe class], dropped);
    ASSERT_TRUE(once);
}

void *made()
{
    return (__bridge_retained void *)[PoolProbe new];
}

// An instance's block, once it is gone: the address the next instance is built at
void *goneAt()
{
    void *at;
    @autoreleasepool {
        PoolProbe *p = [PoolProbe new];
        p->_word = 7;
        at = (__bridge void *)p;
    }
    return at;
}

TEST(ObjectPool, TheNextInstanceIsBuiltOnTheLastOnesZeroedBlock)
{
    pooled();
    void *at = goneAt();
#if __has_feature(address_sanitizer)
    EXPECT_TRUE(__asan_address_is_poisoned(at)) << "a kept block faults under ASan as freed memory would";
#endif
    PoolProbe *next = [PoolProbe new];
    EXPECT_EQ((__bridge void *)next, at);
    EXPECT_EQ(next->_word, 0u);
    EXPECT_EQ(next->_strong, nil);
    EXPECT_TRUE(next->_tracked.values.empty());
    EXPECT_EQ(object_getClass(next), [PoolProbe class]);
}

// As an app that keeps an encoder past endEncoding: its block stays out of the pool until it lets it go
TEST(ObjectPool, AnInstanceStillHeldKeepsItsBlock)
{
    pooled();
    PoolProbe *held = [PoolProbe new];
    void *heldAt = (__bridge void *)held;
    for (int i = 0; i < 200; i++)
        @autoreleasepool {
            PoolProbe *p = [PoolProbe new];
            EXPECT_NE((__bridge void *)p, heldAt);
        }
    held = nil;
    PoolProbe *next = [PoolProbe new];
    EXPECT_EQ((__bridge void *)next, heldAt);
}

// Core Animation's frames: six at a time, a thousand times, after one warm frame, make no block
TEST(ObjectPool, WarmFramesMakeNoBlocks)
{
    pooled();
    constexpr int kPasses = 6;
    auto frame = [] {
        PoolProbe *passes[kPasses];
        for (PoolProbe *__strong &p : passes)
            p = [PoolProbe new];
        for (PoolProbe *__strong &p : passes)
            p = nil;
    };
    frame();
    const uint64_t warm = rawPoolMade([PoolProbe class]);
    for (int i = 0; i < 1000; i++)
        frame();
    EXPECT_EQ(rawPoolMade([PoolProbe class]), warm);
}

// A pool keeps 32 blocks: 40 let go at once free 8, and the next 40 take the 32 and allocate 8 (a miss never waits)
TEST(ObjectPool, PastItsKeepAPoolFreesAndAMissAllocates)
{
    pooled();
    constexpr int kAtOnce = 40, kKept = 32;
    auto hold = [] {
        std::vector<PoolProbe *> live;
        for (int i = 0; i < kAtOnce; i++)
            live.push_back([PoolProbe new]);
    };
    hold();
    const uint64_t made = rawPoolMade([PoolProbe class]), freed = rawPoolFreed([PoolProbe class]);
    hold();
    EXPECT_EQ(rawPoolMade([PoolProbe class]) - made, uint64_t(kAtOnce - kKept));
    EXPECT_EQ(rawPoolFreed([PoolProbe class]) - freed, uint64_t(kAtOnce - kKept));
}

TEST(ObjectPool, DeallocRunsDroppedAndDestroysTheInstanceInPlace)
{
    pooled();
    gDropped = 0;
    gDestroyed = 0;
    static char key;
    __weak id toProbe, toStrong, toAssociated;
    @autoreleasepool {
        PoolProbe *p = [PoolProbe new];
        p->_tracked.values.push_back(1);
        NSObject *strong = [NSObject new];
        p->_strong = strong;
        p->_weak = strong;
        toStrong = strong;
        NSObject *associated = [NSObject new];
        objc_setAssociatedObject(p, &key, associated, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        toAssociated = associated;
        toProbe = p;
    }
    EXPECT_EQ(gDropped.load(), 1);
    EXPECT_EQ(gDestroyed.load(), 1) << "its C++ ivars were destroyed";
    EXPECT_EQ(toProbe, nil) << "weak references to it were cleared";
    EXPECT_EQ(toStrong, nil) << "its ARC ivars were released";
    EXPECT_EQ(toAssociated, nil) << "its associated objects were released";
}

TEST(ObjectPool, ASubclassAllocatesAndFreesAsNSObjectDoes)
{
    pooled();
    const uint64_t before = rawPoolMade([PoolProbe class]);
    gDropped = 0;
    @autoreleasepool {
        PoolProbeChild *c = [PoolProbeChild new];
        c->_more = 3;
        EXPECT_EQ(object_getClass(c), [PoolProbeChild class]);
    }
    EXPECT_EQ(gDropped.load(), 1) << "the pooled class's dealloc work still runs";
    EXPECT_EQ(rawPoolMade([PoolProbe class]), before);
}

// A command buffer an app holds past its completion, with a weak reference and an associated object on it: no instance is
// built on its block while it is held; once it goes its recording goes back first, and the one built on its block next
// has no associated object, while the old weak reference stays nil
TEST(ObjectPool, ACommandBufferHeldIsNeverReusedAndItsBlockComesBackWithNothingOfIt)
{
    static const bool once = rawPoolInstances([PoolBufferProbe class], recordingGiven);
    ASSERT_TRUE(once);
    static char key;
    PoolBufferProbe *held = [PoolBufferProbe new];
    held->_recording.push_back(1);
    void *heldAt = (__bridge void *)held;
    __weak PoolBufferProbe *toHeld = held;
    __weak id toAssociated;
    @autoreleasepool {
        NSObject *associated = [NSObject new];
        objc_setAssociatedObject(held, &key, associated, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        toAssociated = associated;
        for (int i = 0; i < 200; i++)
            @autoreleasepool {
                PoolBufferProbe *b = [PoolBufferProbe new];
                EXPECT_NE((__bridge void *)b, heldAt);
            }
        EXPECT_EQ(toHeld, held);
        EXPECT_NE(toAssociated, nil);
    }
    const int given = gRecordingsGiven;
    held = nil;
    EXPECT_EQ(gRecordingsGiven - given, 1);
    @autoreleasepool {
        EXPECT_EQ(toHeld, nil);
        EXPECT_EQ(toAssociated, nil);
        PoolBufferProbe *next = [PoolBufferProbe new];
        EXPECT_EQ((__bridge void *)next, heldAt);
        EXPECT_EQ(objc_getAssociatedObject(next, &key), nil);
        EXPECT_EQ(toHeld, nil) << "a weak reference to the one gone does not reach the one built on its block";
        EXPECT_TRUE(next->_recording.empty());
    }
}

// Made on one thread and let go on others, as the submit worker and completion threads drop their last references
TEST(ObjectPool, InstancesLetGoOnOtherThreadsComeBack)
{
    pooled();
    constexpr int kThreads = 4, kRounds = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++)
        threads.emplace_back([] {
            for (int i = 0; i < kRounds; i++)
                @autoreleasepool {
                    PoolProbe *p = (__bridge_transfer PoolProbe *)made();
                    p->_tracked.values.push_back(i);
                    std::thread([q = p] {
                        q->_word = 1;
                    }).join();
                }
        });
    for (std::thread &t : threads)
        t.join();
    PoolProbe *after = [PoolProbe new];
    EXPECT_EQ(object_getClass(after), [PoolProbe class]);
    EXPECT_TRUE(after->_tracked.values.empty());
}

}  // namespace
