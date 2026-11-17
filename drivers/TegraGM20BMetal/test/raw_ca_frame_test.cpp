// SPDX-License-Identifier: MIT
// Core Animation's frame on the render server's channel: MetalContext::flush commits its command buffers one at a time,
// each with completed handlers, the submit thread idle after each, the last writing the scanout, then the display's
// present asks for that producer's interrupt (gpu_present.c tgm_chan_need_tokens). The runtime's rule
// (raw_completion_source.h quietEnd, frameHeld, completionLook) over the kernel's (SwitchGpuQuiet.h): the interrupts,
// the interrupt passes, the completion thread's wakes, the owner's wake-entry thread calls, and the handlers' order.
#include <gtest/gtest.h>
#include <deque>
#include <vector>
#include "../raw/runtime/raw_completion_source.h"
#include "../../TegraGM20B/SwitchGpuJobFences.h"
#include "../../TegraGM20B/SwitchGpuQuiet.h"

namespace {
constexpr uint64_t kMs = 1000000;

struct Channel {
    struct Range {
        uint32_t seqno;
        bool quiet;
        uint64_t frameDue;
        unsigned buffer;
    };

    explicit Channel(bool compositorRule) : compositorRule(compositorRule)
    {
        switch_gpu_quiet_init(&quiet, 0);
        switch_gpu_jobs_init(&jobs, 0);
    }

    // One command buffer through MetalContext::flush: its range asked quiet by the runtime, kept loud by the kernel when
    // it closes the scanout's writes (TegraGM20BExecReplyLoud), then the submit thread idles.
    uint32_t commit(unsigned buffer, bool handlers, bool scanout)
    {
        raw::RangeEnd end;
        end.observed = handlers;
        end.handlersOnly = handlers;
        end.compositor = compositorRule;
        const bool asked = raw::quietEnd(end);
        const bool quietEnd = switch_gpu_quiet_end(asked, 0, scanout, 1) != 0;
        const int announce = switch_gpu_quiet_announces(&quiet, quietEnd);
        const uint32_t seqno = ++submitted;
        switch_gpu_jobs_kick(&jobs, seqno, seqno, 0, !announce);
        if (announce)
            switch_gpu_quiet_announce(&quiet, seqno);
        fifo.push_back({seqno, announce != 0});
        const bool rangeQuiet = asked && quietEnd;
        inflight.push_back({seqno, rangeQuiet, rangeQuiet && raw::frameHeld(end) ? now + raw::kFrameEndNs : 0, buffer});
        completionThread();
        return seqno;
    }

    // RawMetalFinish.mm completionLoop: it sleeps in the kernel on its target, which asks for a quiet one's interrupt
    void completionThread()
    {
        if (sleepingOn)
            return;
        const raw::CompletionLook look = raw::completionLook(inflight, false, waiters != 0, now);
        if (!look.target)
            return;
        sleepingOn = inflight[*look.target].seqno;
        ask(sleepingOn);
    }

    // tgm_chan_need: a seqno no pushed interrupt follows gets a wake entry from the owner's thread call
    void ask(uint32_t seqno)
    {
        if (!switch_gpu_quiet_need(&quiet, seqno) || !switch_gpu_quiet_wake_due(&quiet, submitted))
            return;
        ++threadCalls;
        switch_gpu_quiet_announce(&quiet, submitted);
        switch_gpu_jobs_announce_newest(&jobs);
        fifo.push_back({0, true});
    }

    void elapse(uint64_t ns)
    {
        now += ns;
        completionThread();
    }

    // The GPU runs what is queued; each entry that interrupts is one interrupt pass, which wakes the completion thread
    // once its target's fence passed, and it retires every range up to the fence in order.
    void run()
    {
        while (!fifo.empty()) {
            const auto [seqno, interrupts] = fifo.front();
            fifo.pop_front();
            if (seqno)
                fence = seqno;
            if (!interrupts)
                continue;
            ++this->interrupts;
            if (!sleepingOn || int32_t(fence - sleepingOn) < 0)
                continue;
            ++wakes;
            sleepingOn = 0;
            while (!inflight.empty() && int32_t(fence - inflight.front().seqno) >= 0) {
                handlers.push_back(inflight.front().buffer);
                inflight.pop_front();
            }
            completionThread();
        }
    }

    bool compositorRule;
    switch_gpu_quiet quiet{};
    switch_gpu_jobs jobs{};
    uint32_t submitted = 0, fence = 0, sleepingOn = 0, waiters = 0;
    uint64_t now = 0;
    std::deque<std::pair<uint32_t, bool>> fifo;
    std::deque<Range> inflight;
    std::vector<unsigned> handlers;
    unsigned interrupts = 0, wakes = 0, threadCalls = 0;
};

// Two command buffers and the present: an offscreen pass's, then the scanout's, 3 ms apart.
void caFrame(Channel &c)
{
    c.commit(1, true, false);
    c.elapse(3 * kMs);
    const uint32_t producer = c.commit(2, true, true);
    c.ask(producer);
    c.run();
}
}  // namespace

TEST(CaFrame, TwoJobsAndAPresentTakeOneInterruptAndOnePass)
{
    Channel c(true);
    caFrame(c);
    EXPECT_EQ(c.interrupts, 1u) << "the scanout's last writer's own";
    EXPECT_EQ(c.wakes, 1u);
    EXPECT_EQ(c.threadCalls, 0u) << "no wake entry: the present finds its producer loud";
    EXPECT_EQ(c.handlers, (std::vector<unsigned>{1, 2}));
    EXPECT_TRUE(c.inflight.empty());
}

TEST(CaFrame, BeforeEachCommandBufferInterrupted)
{
    Channel c(false);
    caFrame(c);
    EXPECT_EQ(c.interrupts, 2u);
    EXPECT_EQ(c.wakes, 2u);
    EXPECT_EQ(c.handlers, (std::vector<unsigned>{1, 2}));
}

// A frame that commits and never presents (an offscreen render): its range waits one display period for a later commit
// of its frame, then asks for its own interrupt.
TEST(CaFrame, AFrameWithNoPresentIsAskedForOnePeriodAfterItsLastCommit)
{
    Channel c(true);
    c.commit(1, true, false);
    c.run();
    EXPECT_EQ(c.interrupts, 0u);
    EXPECT_TRUE(c.handlers.empty());
    c.elapse(raw::kFrameEndNs - 1);
    EXPECT_EQ(c.threadCalls, 0u);
    c.elapse(1);
    c.run();
    EXPECT_EQ(c.threadCalls, 1u);
    EXPECT_EQ(c.interrupts, 1u);
    EXPECT_EQ(c.handlers, (std::vector<unsigned>{1}));
}

// A thread in waitUntilCompleted does not wait for the frame: the range is asked for at once.
TEST(CaFrame, AWaiterTakesTheRangeAtOnce)
{
    Channel c(true);
    c.commit(1, true, false);
    c.waiters = 1;
    c.completionThread();
    c.run();
    EXPECT_EQ(c.interrupts, 1u);
    EXPECT_EQ(c.handlers, (std::vector<unsigned>{1}));
}

// The rule is the compositor's: an app's command buffer with handlers and nothing behind it keeps its own interrupt.
TEST(CaFrame, AnAppsCommandBufferKeepsItsInterrupt)
{
    raw::RangeEnd end;
    end.observed = true;
    end.handlersOnly = true;
    EXPECT_FALSE(raw::quietEnd(end));
    end.compositor = true;
    EXPECT_TRUE(raw::quietEnd(end));
    EXPECT_TRUE(raw::frameHeld(end));
    end.handlersOnly = false;
    EXPECT_FALSE(raw::quietEnd(end)) << "a present or a waiter";
}
