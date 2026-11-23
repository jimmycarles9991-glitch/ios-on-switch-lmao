// SPDX-License-Identifier: MIT
// Core Animation shaped command buffers recorded at the render thread's pace (scale_frames.h), which the submit worker
// encodes ahead of their commits (TegraGM20BMetalPipeline.mm), that fail or meet no channel: a failure recorded stops the encode
// ahead; one the commit finds is judged at the command buffer's turn, which gives the encode ahead back before it
// completes; a channel the kernel refuses an encode ahead is asked for again by the commit's encode once the kernel's
// channel epoch moved.
#include "scale_frames.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace {

using scale_frames::opened;

const scale_frames::Shape kShape{15, 90, 4};

NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, sel_registerName("switchMetalPipelined"));
}

std::string path(id<MTLCommandBuffer> cb)
{
    return [[(id)cb performSelector:@selector(switchMetalPath)] UTF8String];
}

long long handoffs()
{
    return [harness::counters(opened().device)[@"pipeline_handoffs"] longLongValue];
}

// kShape recorded on `queue` a pass every millisecond, so the worker is handed its ended encoders and encodes them
// ahead; `after` runs once pass `p` ended, with that pass's encoder
id<MTLCommandBuffer> paced(id<MTLCommandQueue> queue, const std::vector<id<MTLTexture>> &into, uint32_t frame,
                           void (^after)(id<MTLCommandBuffer>, int, id<MTLRenderCommandEncoder>) = nil)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < kShape.passes; p++) {
        __block id<MTLRenderCommandEncoder> encoder = nil;
        scale_frames::pass(cb, opened().state, into, kShape, p, frame, nil, ^(id<MTLRenderCommandEncoder> e) {
            encoder = e;
        });
        if (after)
            after(cb, p, encoder);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cb;
}

// Refused: every free slot another driver's, the device's channel killed, and the slot it frees another driver's too,
// so its next begin gets no channel (facade_errors_test.cpp's kernelRefusesTheChannel). Undone: the other driver's
// channels destroyed, which moves the kernel's channel epoch, and the next begin makes one again.
void refuseChannels(bool refused)
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    kernel_model::Model &m = kernel_model::model;
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (!m.slots[s].uid)
            m.slots[s].foreign = refused;
    m.foreignTakesFreedSlot = refused;
    if (!refused) {
        m.channelEpochMoves();
        return;
    }
    for (uint32_t s = 0; s < kernel_model::kSlots; s++)
        if (m.slots[s].uid && !m.slots[s].foreign)
            m.kill(s + 1, 0x1f);
}

// The kernel refuses the channel while two frames record: the worker's encode ahead of each meets no channel and gives
// the frame back (VetoChannel), failing neither. The first is committed with the channel still refused and fails at its
// own encode (enc.nochannel); the second is committed once the kernel makes channels again and completes on raw. The
// worker is held through the second's recording, as on a loaded host, and each frame waits for the worker's give-back
// before the channel changes or the commit.
void anEncodeAheadWithNoChannelLeavesTheFailureToTheCommit()
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, kShape);
    id<MTLCommandBuffer> settled = paced(queue, into, 0);
    [settled commit];
    [settled waitUntilCompleted];
    ASSERT_EQ(settled.status, MTLCommandBufferStatusCompleted);
    refuseChannels(true);
    id<MTLCommandBuffer> refused = paced(queue, into, 1);
    EXPECT_NE(scale_frames::waitAhead(refused) & uint32_t(VetoChannel), 0u);
    [refused commit];
    [refused waitUntilCompleted];
    EXPECT_EQ(refused.status, MTLCommandBufferStatusError);
    EXPECT_EQ(path(refused), "error:enc.nochannel");
    NSDictionary *p = pipelined(refused);
    ASSERT_NE(p, nil);
    EXPECT_NE([p[@"given_back"] intValue] & int(VetoChannel), 0) << [[p description] UTF8String];
    scale_frames::WorkerHold hold;
    id<MTLCommandBuffer> back = paced(queue, into, 2);
    hold.release();
    EXPECT_NE(scale_frames::waitAhead(back) & uint32_t(VetoChannel), 0u);
    refuseChannels(false);
    [back commit];
    [back waitUntilCompleted];
    EXPECT_EQ(back.status, MTLCommandBufferStatusCompleted) << back.error.description.UTF8String;
    EXPECT_EQ(path(back), "gm20b");
    NSDictionary *q = pipelined(back);
    ASSERT_NE(q, nil);
    EXPECT_NE([q[@"given_back"] intValue] & int(VetoChannel), 0) << [[q description] UTF8String];
}

}  // namespace

TEST(GM20BFacadeAheadFailures, AnEncodeAheadWithNoChannelLeavesTheFailureToTheCommit)
{
    switch_ios::test::ExpectPassesInFreshProcess(anEncodeAheadWithNoChannelLeavesTheFailureToTheCommit);
}

// Pass 9 of 15 ends twice, Metal's validation error, which fails the command buffer as it records: the worker is handed
// none of the passes after it, and the failure is the veto it reads at its next look
TEST(GM20BFacadeAheadFailures, AFailureAsItRecordsHandsTheWorkerNothingMore)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, kShape);
    __block long long atFailure = -1;
    id<MTLCommandBuffer> cb = paced(queue, into, 0, ^(id<MTLCommandBuffer>, int p, id<MTLRenderCommandEncoder> e) {
        if (p != 9)
            return;
        [e endEncoding];
        atFailure = handoffs();
    });
    EXPECT_EQ(handoffs() - atFailure, 0) << "hand-offs after the failure";
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_NE([p[@"veto"] intValue] & int(VetoRefused), 0) << [[p description] UTF8String];
}

// Pass 15 of 15 draws once more after a blit encoder began, Metal's validation error the commit finds
// (CommandAfterOtherWork): the commit fails the command buffer InvalidResource with nothing vetoing the encode ahead the
// worker still holds. Its turn gives that back before its completion returns the recording the encode reads: the next
// command buffer is encoded ahead as any other.
TEST(GM20BFacadeAheadFailures, ACommandBufferTheCommitFailsGivesTheDeviceBackBeforeItCompletes)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, kShape);
    id<MTLCommandBuffer> cb = paced(queue, into, 0, ^(id<MTLCommandBuffer> c, int p, id<MTLRenderCommandEncoder> e) {
        if (p != kShape.passes - 1)
            return;
        id<MTLBlitCommandEncoder> blit = [c blitCommandEncoder];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [blit endEncoding];
    });
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u) << "the worker gave back the encode ahead its turn gives back";
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"veto"] intValue], 0) << [[p description] UTF8String];
    id<MTLCommandBuffer> next = paced(queue, into, 1);
    EXPECT_EQ(scale_frames::waitAhead(next), 0u);
    [next commit];
    [next waitUntilCompleted];
    ASSERT_EQ(next.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(next), "gm20b");
    NSDictionary *q = pipelined(next);
    ASSERT_NE(q, nil);
    EXPECT_GE([q[@"pipelined"] intValue], 10) << [[q description] UTF8String];
}
