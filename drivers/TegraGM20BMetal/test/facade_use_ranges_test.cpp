// SPDX-License-Identifier: MIT
// gpu.path.use_ranges' two arms (hw_test_app GPUPathTests.swift UseRangesPathTest) on the facade over the kernel model.
// Shared buffers live in VOL=1 write-combined chunks, resource-set members no CPU lease takes out, so a fill of one
// lists no use of its own: the set names the chunk. A Shared heap's buffers share its arena, which the lease at each
// buffer's creation takes out of the set, so a fill lists its buffer by that buffer's bytes and nothing past them.
#include "encode_cost_shape.h"
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace {

using scale_frames::opened;

constexpr NSUInteger kLength = 768, kBuffers = 8;

long useAdds()
{
    return [(id)opened().device switchMetalEncodeProfile][@"use_adds"].longValue;
}

// A blit fill of every other buffer, each in a command buffer of its own, all completed
void fillEveryOther(NSArray<id<MTLBuffer>> *buffers)
{
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    NSMutableArray<id<MTLCommandBuffer>> *done = [NSMutableArray array];
    for (NSUInteger i = 0; i < buffers.count; i += 2) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b fillBuffer:buffers[i] range:NSMakeRange(0, kLength) value:uint8_t(0x80 | i)];
        [b endEncoding];
        [cb commit];
        [done addObject:cb];
    }
    for (id<MTLCommandBuffer> cb in done) {
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }
}

// The uses the model's jobs from `first` on list by address with a byte range (the set's members follow them, never
// ranged), and the most bytes one names
struct Ranged {
    size_t uses = 0;
    uint64_t widest = 0;
};

Ranged rangedSince(size_t first)
{
    Ranged r;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j)
        for (const TegraGM20BAbiSurfaceUse &u : kernel_model::model.jobs[j].uses)
            if (u.access & TegraGM20BSurfaceRange) {
                r.uses++;
                r.widest = std::max<uint64_t>(r.widest, u.reserved);
            }
    return r;
}

TEST(GM20BFacadeUseRanges, SharedBuffersAreNamedByTheSetAndListNoUseOfTheirOwn)
{
    ASSERT_NE(opened().device, nil);
    NSMutableArray<id<MTLBuffer>> *shared = [NSMutableArray array];
    for (NSUInteger i = 0; i < kBuffers; i++)
        [shared addObject:[opened().device newBufferWithLength:kLength options:MTLResourceStorageModeShared]];
    const long before = useAdds();
    const size_t first = kernel_model::model.jobs.size();
    fillEveryOther(shared);
    EXPECT_EQ(useAdds() - before, 0) << "a Shared buffer's fill listed a use of its own";
    EXPECT_EQ(rangedSince(first).uses, 0u);
    ASSERT_GT(kernel_model::model.jobs.size(), first);
}

TEST(GM20BFacadeUseRanges, ASharedHeapsBuffersAreEachListedByTheirOwnBytes)
{
    ASSERT_NE(opened().device, nil);
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = 64 << 10;
    hd.storageMode = MTLStorageModeShared;
    id<MTLHeap> heap = [opened().device newHeapWithDescriptor:hd];
    ASSERT_NE(heap, nil);
    NSMutableArray<id<MTLBuffer>> *inHeap = [NSMutableArray array];
    for (NSUInteger i = 0; i < kBuffers; i++) {
        id<MTLBuffer> b = [heap newBufferWithLength:kLength options:MTLResourceStorageModeShared];
        ASSERT_NE(b, nil);
        [inHeap addObject:b];
    }
    const long before = useAdds();
    const size_t first = kernel_model::model.jobs.size();
    fillEveryOther(inHeap);
    EXPECT_GE(useAdds() - before, long(kBuffers / 2)) << "the heap's fills listed no use";
    const Ranged ranged = rangedSince(first);
    EXPECT_GE(ranged.uses, kBuffers / 2) << "the heap's buffers were not listed by byte range";
    EXPECT_EQ(ranged.widest, kLength) << "a fill listed more than its buffer's bytes";
}

}  // namespace
