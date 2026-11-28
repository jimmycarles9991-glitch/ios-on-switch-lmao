// SPDX-License-Identifier: MIT
// A new Private buffer reads zeros as Metal's does, through the facade over the kernel model: one made on a span a freed
// Private buffer left dirty, in a chunk no host-visible buffer reached (so no CPU zeroed it), is filled by the copy engine
// ahead of the first submission that names it (TegraGM20BMetalFresh.h establishFreshBuffer), counted in switchMetalFresh.
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"
#include "../TegraGM20BMetalObjects.h"

#include <gtest/gtest.h>

namespace {

constexpr NSUInteger kBytes = 64 * 1024;

uint64_t filled(id<MTLDevice> device, NSString *key)
{
    return [harness::ask(device, "switchMetalFresh")[key] unsignedLongLongValue];
}

void commitAndWait(id<MTLCommandBuffer> cb)
{
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
}

void aPrivateBufferOnADirtySpanIsFilledBeforeItsFirstUse()
{
    const std::string pack = harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm));
    id<MTLDevice> device = harness::open(pack);
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    uint64_t va = 0;
    @autoreleasepool {
        id<MTLBuffer> first = [device newBufferWithLength:kBytes options:MTLResourceStorageModePrivate];
        ASSERT_NE(first, nil);
        va = ((SwitchMetalBuffer *)first)->_raw.va;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b fillBuffer:first range:NSMakeRange(0, kBytes) value:0xab];
        [b endEncoding];
        commitAndWait(cb);
    }
    id<MTLBuffer> second = nil;
    for (int tries = 0; tries < 8 && !second; tries++) {
        id<MTLBuffer> made = [device newBufferWithLength:kBytes options:MTLResourceStorageModePrivate];
        ASSERT_NE(made, nil);
        if (((SwitchMetalBuffer *)made)->_raw.va == va)
            second = made;
    }
    ASSERT_NE(second, nil) << "no new Private buffer took the freed one's span";
    EXPECT_TRUE(((SwitchMetalBuffer *)second)->_raw.freshFill);

    const uint64_t before = filled(device, @"filled"), bytesBefore = filled(device, @"filled_bytes");
    id<MTLBuffer> out = [device newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:second sourceOffset:0 toBuffer:out destinationOffset:0 size:kBytes];
    [b endEncoding];
    commitAndWait(cb);
    EXPECT_EQ(filled(device, @"filled") - before, 1u)
        << "the buffer's first use read the span as its last owner left it";
    EXPECT_EQ(filled(device, @"filled_bytes") - bytesBefore, uint64_t(kBytes));

    const uint64_t again = filled(device, @"filled");
    id<MTLCommandBuffer> next = [queue commandBuffer];
    id<MTLBlitCommandEncoder> n = [next blitCommandEncoder];
    [n copyFromBuffer:second sourceOffset:0 toBuffer:out destinationOffset:0 size:kBytes];
    [n endEncoding];
    commitAndWait(next);
    EXPECT_EQ(filled(device, @"filled"), again) << "filled again at a later use";
}

TEST(GM20BFacadeFreshBuffer, APrivateBufferOnADirtySpanIsFilledBeforeItsFirstUse)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPrivateBufferOnADirtySpanIsFilledBeforeItsFirstUse);
}

}  // namespace
