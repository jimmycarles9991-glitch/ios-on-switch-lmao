// SPDX-License-Identifier: MIT
// A command buffer waiting on a shared event whose producer, another queue's raw work, is still pending on a channel
// the kernel killed after it ran: the waiting range's encode makes the channel again, finds the producer outside its
// GPU order (TegraGM20BMetalEncode.mm producersOrdered) and waits for its outcome, which wakes its queue. The producer that
// ran before the kill lets it run on raw; one the kill took fails it. Nothing goes to the m2v device.
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <string>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;
constexpr float kTriangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

Opened openPacked()
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    return {device, [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error]};
}

void drawInto(id<MTLCommandBuffer> cb, const Opened &o)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = [o.device newTextureWithDescriptor:d];
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:o.state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

constexpr int64_t kBound = 10 * int64_t(NSEC_PER_SEC);

bool waited(dispatch_semaphore_t s)
{
    return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, kBound)) == 0;
}

// The model's GPU for this scenario: every job completes as the kernel takes it but the one `hold` names (the
// producer's), whose completion thread waits in WaitFence until `gate`. The first job of the channel made again (its
// context) is `reopened`; the next one (a third queue's, committed after that) opens the gate. By then the waiting
// command buffer's queue has been visited on the new channel: its producer was pending, outside the new channel's
// order. Never destroyed: the model's hooks and the completion thread reach it until the process exits.
struct HeldProducer {
    std::atomic<bool> hold{false}, reopenSeen{false}, released{false};
    std::atomic<uint32_t> channel{0}, seqno{0};
    unsigned createsAtKill = 0;
    dispatch_semaphore_t reopened = dispatch_semaphore_create(0), gate = dispatch_semaphore_create(0);

    void install(kernel_model::Model &m)
    {
        m.jobAdded = [this, &m] {
            const kernel_model::Job &job = m.jobs.back();
            if (hold.exchange(false)) {
                channel = job.channel;
                seqno = job.seqno;
                return;
            }
            m.fence(job.channel - 1).completed = job.seqno;
            if (!createsAtKill || m.creates == createsAtKill || released)
                return;
            if (!reopenSeen.exchange(true)) {
                dispatch_semaphore_signal(reopened);
                return;
            }
            released = true;
            dispatch_semaphore_signal(gate);
        };
        m.blockUntil = [this](uint32_t onChannel, uint32_t onSeqno) {
            if (onChannel == channel && onSeqno == seqno && !released)
                (void)waited(gate);
            return true;
        };
    }

    // The kernel kills the producer's channel, after its job ran (`ran`) or with it running
    void kill(kernel_model::Model &m, bool ran)
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        if (ran)
            m.fence(channel - 1).completed = seqno;
        createsAtKill = m.creates;
        m.kill(channel, 0x1f);
    }
};

void producerPendingOnAKilledChannel(bool producerRan)
{
    const Opened o = openPacked();
    ASSERT_TRUE(o.device != nil && o.state != nil);
    kernel_model::Model &m = kernel_model::model;
    HeldProducer &gpu = *new HeldProducer;
    gpu.install(m);
    id<MTLCommandQueue> producerQueue = [o.device newCommandQueue], waiterQueue = [o.device newCommandQueue];
    id<MTLCommandBuffer> first = [producerQueue commandBuffer];
    drawInto(first, o);
    [first commit];
    [first waitUntilCompleted];
    ASSERT_EQ(first.status, MTLCommandBufferStatusCompleted) << text(first.error);
    NSDictionary *before = counters(o.device);

    id<MTLSharedEvent> event = [o.device newSharedEvent];
    id<MTLCommandBuffer> producer = [producerQueue commandBuffer];
    drawInto(producer, o);
    [producer encodeSignalEvent:event value:1];
    gpu.hold = true;
    [producer commit];
    [producer waitUntilScheduled];
    ASSERT_NE(gpu.seqno.load(), 0u) << "the producer's job is held";
    gpu.kill(m, producerRan);

    id<MTLCommandBuffer> waiter = [waiterQueue commandBuffer];
    [waiter encodeWaitForEvent:event value:1];
    drawInto(waiter, o);
    [waiter commit];
    ASSERT_TRUE(waited(gpu.reopened)) << "the waiting range's encode did not make the channel again";
    id<MTLCommandBuffer> third = [[o.device newCommandQueue] commandBuffer];
    drawInto(third, o);
    [third commit];
    [third waitUntilScheduled];
    [waiter waitUntilCompleted];
    [producer waitUntilCompleted];
    [third waitUntilCompleted];

    EXPECT_TRUE(gpu.released.load()) << "no job after the new channel's context opened the gate";
    EXPECT_EQ(third.status, MTLCommandBufferStatusCompleted) << text(third.error);
    NSDictionary *after = counters(o.device);
    if (producerRan) {
        EXPECT_EQ(producer.status, MTLCommandBufferStatusCompleted) << text(producer.error);
        EXPECT_EQ(event.signaledValue, 1u);
        EXPECT_EQ(waiter.status, MTLCommandBufferStatusCompleted) << text(waiter.error);
    } else {
        EXPECT_EQ(producer.status, MTLCommandBufferStatusError);
        EXPECT_EQ(waiter.status, MTLCommandBufferStatusError);
        EXPECT_EQ(text(waiter.error.localizedDescription), "A GPU event producer failed") << text(waiter.error);
        // the kill's error, from the completion thread, and the waiter's, from its readiness: neither names a predicate
        EXPECT_EQ(gm20bPath(producer), "error:none");
        EXPECT_EQ(gm20bPath(waiter), "error:none");
        EXPECT_EQ(failedMoved(before, after), (std::map<std::string, long long>{{"failed", 2}, {"failed.none", 2}}))
            << text(after);
    }
}

}  // namespace

TEST(GM20BFacadeProducer, AWaitOnAProducerThatRanBeforeItsChannelWasKilledRunsOnRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        producerPendingOnAKilledChannel(true);
    });
}

TEST(GM20BFacadeProducer, AWaitOnAProducerTheKillTookFailsAsItsProducerDid)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        producerPendingOnAKilledChannel(false);
    });
}
