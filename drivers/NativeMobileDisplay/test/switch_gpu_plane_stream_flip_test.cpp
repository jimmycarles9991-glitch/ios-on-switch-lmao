// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchGpuPlaneStream.h in the media completion flip's model (switch_media_completion_flip_harness.h), with a bridge
// that fails: the stream route is a jobless bridge's alone and watched from its submission, and the set's verdict
// follows what the hardware did. A set the stream wrote is shown, with one line when a client's failure is dated after
// its writes; a set the failure cancelled first is not shown. Only a compositor's kill or a loss nothing reports panics.
// Everything else completes through the bridge.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "switch_media_completion_flip_harness.h"

namespace media_flip_test {
namespace {

constexpr uint32_t kStartB = Display::kWinbufBBase + native_planes::kStartAddr;

class GpuPlaneStreamFlip : public testing::Test {
protected:
    Display d;
    uint64_t seq = 0;

    void SetUp() override
    {
        gLog.clear();
        gFenceResult = gWaitResult = 0;
        gFences = 0;
        gBridge = {};
        for (bool &told : Display::planeStreamTold)
            told = false;
        d.stats = true;
        shownByGate(video(1));
    }

    void TearDown() override
    {
        gWaitResult = 0;
    }

    void frame()
    {
        d.model.boundary();
        d.frameEnd();
    }

    uint32_t address(int slot) const
    {
        return d.windowAddress(video(slot).window[0], 0);
    }

    void shownByGate(const Display::PlaneSet &p)
    {
        ASSERT_TRUE(d.offer(p, ++seq));
        d.complete(0);
        d.donePass();
        frame();
        ASSERT_EQ(d.shown.back(), seq);
    }

    void settle()
    {
        d.donePass();
        for (int i = 0; i < 3; ++i)
            ASSERT_NO_THROW(frame());
    }

    bool logged(const char *text) const
    {
        for (const auto &line : gLog)
            if (line.find(text) != std::string::npos)
                return true;
        return false;
    }
};

TEST_F(GpuPlaneStreamFlip, AJoblessSetStreamsWatchedFromItsSubmission)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    ASSERT_EQ(gHost.owner, &d.mediaHostStream);
    EXPECT_EQ(gBridge.watches, 1u);
    EXPECT_TRUE(gBridge.watchedAtGate) << "the hang check covers the producer before the stream can wait on it";
    EXPECT_EQ(d.stPlaneGpuStreamed, 1u);
    d.filter();
    d.complete(0);
    settle();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

// Written: the producer's client went away after its writes reached DRAM. The frame is whole and on the panel.
TEST_F(GpuPlaneStreamFlip, AWrittenSetWhoseProducerFailsAfterItsWritesIsShownWithOneLine)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    d.filter();
    ASSERT_TRUE(d.mediaFlip.early());
    gBridge.status = -5;
    gBridge.why = SWITCH_GPU_BACKING_WRITER_GONE;
    ASSERT_NO_THROW(d.complete(0));
    EXPECT_EQ(d.mediaHostStream.bridgeWhy, SWITCH_GPU_BACKING_WRITER_GONE);
    settle();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.notShown, 0u);
    EXPECT_TRUE(logged("shown as done"));
}

// The same with no filter waiter free: the stream's completion claims the set it wrote, and the verdict keeps it.
TEST_F(GpuPlaneStreamFlip, AWrittenSetWithNoFilterIsShownToo)
{
    gWaitResult = -16;
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    gBridge.status = -5;
    gBridge.why = SWITCH_GPU_BACKING_WRITE_REFUSED;
    ASSERT_NO_THROW(d.complete(0));
    settle();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.notShown, 0u);
}

// Not written: the failure's release cancelled the stream before its syncpoint passed. The panel keeps the set before.
TEST_F(GpuPlaneStreamFlip, ASetWhoseProducerFailsBeforeItsWritesIsNotShown)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    d.bridgeFails(SWITCH_GPU_BACKING_WRITER_GONE);
    ASSERT_NO_THROW(d.complete(0));
    EXPECT_EQ(d.gpuStatus, SWITCH_GPU_BACKING_WRITER_GONE) << "the completion says why";
    settle();
    EXPECT_EQ(d.notShown, 1u);
    EXPECT_EQ(d.model.active[kStartB], address(1));
    EXPECT_EQ(gBridge.watches, 1u);
}

// A failure stored before the stream's publication: host1x refuses it, and the set completes through its bridge.
TEST_F(GpuPlaneStreamFlip, AFailureBeforeThePublicationCompletesThroughTheBridgeNotShown)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    gBridge.failed = gBridge.why = SWITCH_GPU_BACKING_WRITER_GONE;
    d.copyThread();
    EXPECT_EQ(gHost.owner, nullptr);
    EXPECT_EQ(d.stPlaneGpuStreamRefused, 1u);
    ASSERT_EQ(gBridge.notifies, 1u);
    EXPECT_EQ(gBridge.releases, 1u);
    ASSERT_NO_THROW(gBridge.completed(gBridge.context, SWITCH_GPU_BACKING_WRITER_GONE));
    settle();
    EXPECT_EQ(d.notShown, 1u);
    EXPECT_EQ(d.model.active[kStartB], address(1));
}

// A bridge with its own GPU job passes over a killed producer's writes, so its set never streams: the bridge's own
// completion decides it, and a failure leaves the set unshown.
TEST_F(GpuPlaneStreamFlip, ABridgeWithAGpuJobCompletesThroughTheBridge)
{
    gBridge.jobless = 0;
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    EXPECT_EQ(gHost.owner, nullptr) << "no stream over a bridge whose failures do not cancel it";
    EXPECT_EQ(d.stPlaneGpuJobful, 1u);
    EXPECT_EQ(gBridge.notifies, 1u);
    EXPECT_EQ(gBridge.releases, 1u);
    EXPECT_TRUE(logged("has a GPU job of its own"));
    ASSERT_NO_THROW(gBridge.completed(gBridge.context, SWITCH_GPU_BACKING_WRITER_GONE));
    settle();
    EXPECT_EQ(d.notShown, 1u);
    EXPECT_EQ(d.model.active[kStartB], address(1));
}

TEST_F(GpuPlaneStreamFlip, ABridgeWithAGpuJobThatSucceedsIsShown)
{
    gBridge.jobless = 0;
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    ASSERT_NO_THROW(gBridge.completed(gBridge.context, 0));
    settle();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

// A mixed set's bridge covers the GPU's producer alone; the stream WAITs on VIC's syncpoint itself.
TEST_F(GpuPlaneStreamFlip, AMixedSetsBridgeCoversTheGpuAloneAndItsStreamWaitsOnVic)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq, true));
    d.copyThread();
    ASSERT_EQ(gBridge.tokens.size(), 1u);
    EXPECT_EQ(gBridge.tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_GM20B));
    ASSERT_EQ(gHost.producers.size(), 1u);
    EXPECT_EQ(gHost.producers[0].engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
}

// A mixed set that does not stream lets its GPU-only bridge go and completes once every producer is done.
TEST_F(GpuPlaneStreamFlip, AMixedSetThatDoesNotStreamWaitsForEveryProducer)
{
    gBridge.jobless = 0;
    ASSERT_TRUE(d.offerGpu(video(2), ++seq, true));
    d.copyThread();
    EXPECT_EQ(gBridge.notifies, 0u);
    EXPECT_EQ(gBridge.releases, 1u);
    ASSERT_EQ(gBridge.fenced.size(), 2u);
    EXPECT_EQ(gBridge.fenced[1].engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    ASSERT_NO_THROW(gBridge.completed(gBridge.context, 0));
    settle();
    EXPECT_EQ(d.shown.back(), seq);
}

// A DC A frame end between the stream's completion and its pass (the gate thread against host1x's completion): the
// written set it takes always has its pass signalled.
TEST_F(GpuPlaneStreamFlip, AFrameEndDuringTheStreamsCompletionFindsItsPassSignalled)
{
    gWaitResult = -16;
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    unsigned frameEnds = 0;
    gDelivered = [this, &frameEnds](void *display, int status) {
        d.frameEnd();
        ++frameEnds;
        static_cast<Display *>(display)->deliver(status);
    };
    ASSERT_NO_THROW(d.complete(0));
    EXPECT_EQ(frameEnds, 1u);
    settle();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

// The two later instants of the completion's order against a frame end: after the pass is signalled but before the
// claim (the flip still armed), and after the claim but before the generation goes (the written flip's pass runs then).
TEST_F(GpuPlaneStreamFlip, AFrameEndAtEitherLaterInstantOfTheCompletionIsRight)
{
    for (const bool afterClaim : {false, true}) {
        SCOPED_TRACE(afterClaim ? "after the claim" : "before the claim");
        gWaitResult = -16;
        ASSERT_TRUE(d.offerGpu(video(afterClaim ? 3 : 2), ++seq));
        d.copyThread();
        unsigned frameEnds = 0;
        if (afterClaim) {
            gDelivered = [this, &frameEnds](void *display, int status) {
                static_cast<Display *>(display)->deliver(status);
                d.frameEnd();
                ++frameEnds;
            };
        } else {
            d.passSignalled = [this, &frameEnds] {
                d.frameEnd();
                ++frameEnds;
            };
        }
        ASSERT_NO_THROW(d.complete(0));
        d.passSignalled = nullptr;
        gDelivered = [](void *display, int status) {
            static_cast<Display *>(display)->deliver(status);
        };
        EXPECT_EQ(frameEnds, 1u);
        settle();
        EXPECT_EQ(d.shown.back(), seq);
        EXPECT_EQ(d.model.active[kStartB], address(afterClaim ? 3 : 2));
    }
}

// A compositor's own GPU fault is a contract our driver or compiler breaks: the written set's verdict panics, as the
// bridge fence's does for it, so the record finds the bug.
TEST_F(GpuPlaneStreamFlip, AWrittenSetWhoseCompositorProducerWasKilledPanics)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    d.filter();
    gBridge.status = -5;
    gBridge.why = SWITCH_GPU_BACKING_WRITER_FAILED;
    EXPECT_THROW(d.complete(0), std::runtime_error);
}

// The other panic: a set host1x wrote whose bridge says its producers finished, and whose frame was then lost.
TEST_F(GpuPlaneStreamFlip, AWrittenSetLostWithNoFailureReportedIsTheKernels)
{
    ASSERT_TRUE(d.offerGpu(video(2), ++seq));
    d.copyThread();
    d.filter();
    EXPECT_THROW(d.complete(-5), std::runtime_error);
}

}  // namespace
}  // namespace media_flip_test
