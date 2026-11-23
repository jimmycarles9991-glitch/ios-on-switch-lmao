// SPDX-License-Identifier: MIT
// counter_kernel.h's resolve kernels run on this Mac's GPU: the byte resolve into a destination offset off 4 bytes
// writes the bytes the word resolve writes into a word, moved there, and leaves every byte around them as it was, as an
// M4 Max's own resolveCounters: writes them (validation off, 3 runs).
#include "drivers/TegraGM20BMetal/counter_kernel.h"

#import <Metal/Metal.h>
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kSamples = 5, kBytes = 256;
constexpr uint8_t kUntouched = 0x22;

id<MTLComputePipelineState> stateOf(id<MTLDevice> dev, const char *name)
{
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(gm20b::counters::kResolveSource) options:nil error:&error];
    EXPECT_NE(library, nil) << error.localizedDescription.UTF8String;
    id<MTLFunction> function = [library newFunctionWithName:@(name)];
    return function ? [dev newComputePipelineStateWithFunction:function error:&error] : nil;
}

// Samples k = 0 .. kSamples - 1 taken 384 k PTIMER units past the table's one anchor, sample 3 of no payload; a buffer of
// kUntouched bytes they resolve into at `offset` by `state` (the byte resolve where `offset` is off 4 bytes), as
// TegraGM20BMetalCounterEncode.mm binds it
std::vector<uint8_t> resolvedAt(id<MTLDevice> dev, id<MTLComputePipelineState> state, uint32_t offset)
{
    std::vector<uint32_t> records(4 * kSamples, 0);
    for (uint32_t k = 0; k < kSamples; k++) {
        records[4 * k] = k == 3 ? 0 : 1;
        records[4 * k + 2] = 0x70000000u + 384 * k;
        records[4 * k + 3] = 2;
    }
    std::vector<uint32_t> anchors(4 + 4 * 63, 0);
    anchors[0] = 1;
    anchors[4] = 0x70000000u;
    anchors[5] = 2;
    anchors[6] = 0x12345678u;
    anchors[7] = 9;
    id<MTLBuffer> in = [dev newBufferWithBytes:records.data() length:records.size() * 4 options:0];
    id<MTLBuffer> table = [dev newBufferWithBytes:anchors.data() length:anchors.size() * 4 options:0];
    id<MTLBuffer> out = [dev newBufferWithLength:kBytes options:0];
    memset(out.contents, kUntouched, kBytes);
    const gm20b::counters::ResolveParams params = {kSamples};
    const gm20b::counters::ResolveBytesParams shifted = {kSamples, offset % 4};
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:state];
    [e setBuffer:in offset:0 atIndex:0];
    [e setBuffer:out offset:offset - offset % 4 atIndex:1];
    if (offset % 4)
        [e setBytes:&shifted length:sizeof shifted atIndex:2];
    else
        [e setBytes:&params length:sizeof params atIndex:2];
    [e setBuffer:table offset:0 atIndex:3];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(gm20b::counters::kResolveThreads, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    const auto *bytes = static_cast<const uint8_t *>(out.contents);
    return std::vector<uint8_t>(bytes, bytes + kBytes);
}

TEST(GM20BCounterResolve, AnOffsetOffFourBytesWritesTheWordResolvesBytesThere)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    ASSERT_NE(dev, nil);
    id<MTLComputePipelineState> words = stateOf(dev, gm20b::counters::kResolveFunction);
    id<MTLComputePipelineState> bytes = stateOf(dev, gm20b::counters::kResolveBytesFunction);
    ASSERT_NE(words, nil);
    ASSERT_NE(bytes, nil);
    const std::vector<uint8_t> word = resolvedAt(dev, words, 8);
    for (uint32_t offset : {9u, 10u, 11u}) {
        const std::vector<uint8_t> moved = resolvedAt(dev, bytes, offset);
        for (uint32_t i = 0; i < kBytes; i++) {
            const bool sample = i >= offset && i < offset + 8 * kSamples;
            EXPECT_EQ(moved[i], sample ? word[i - offset + 8] : kUntouched) << "offset " << offset << " byte " << i;
        }
    }
    uint64_t first = 0;
    memcpy(&first, &word[8], 8);
    EXPECT_EQ(first, 0x912345678ull) << "the anchor's own sample resolves to the anchor's time";
}

}  // namespace
