// SPDX-License-Identifier: MIT
// The completion source (raw/runtime/raw_completion_source.h) over the kernel's quiet job ends (TegraGM20B
// SwitchGpuQuiet.h): which jobs end quiet, which range the completion thread sleeps on, and, for whole frames, the
// interrupts and completion-thread wakes before (every job interrupts, one sleep per range) and after.
#include <gtest/gtest.h>
#include <deque>
#include <vector>
#include "../raw/runtime/raw_completion_source.h"
#include "../../TegraGM20B/SwitchGpuJobFences.h"
#include "../../TegraGM20B/SwitchGpuQuiet.h"

namespace {
struct Range {
    uint32_t seqno;
    bool quiet;
};

TEST(QuietEnd, OnlyWorkInHandBehindItAndNobodyWaitingMakesARangeQuiet)
{
    raw::RangeEnd end;
    EXPECT_FALSE(raw::quietEnd(end)) << "a command buffer's last range with nothing behind it";
    end.followed = true;
    EXPECT_TRUE(raw::quietEnd(end));
    end.observed = true;
    EXPECT_FALSE(raw::quietEnd(end)) << "completed handlers, a present or waitUntilCompleted";
    end = {};
    end.lastOfBuffer = false;
    EXPECT_FALSE(raw::quietEnd(end)) << "its next range is an event's or m2v's";
    end.nextRangeRaw = true;
    EXPECT_TRUE(raw::quietEnd(end));
    end.signals = true;
    EXPECT_FALSE(raw::quietEnd(end)) << "the CPU publishes its signals when it completes";
    end.signals = false;
    end.cpuOrdered = true;
    EXPECT_FALSE(raw::quietEnd(end));
}

TEST(CompletionTarget, TheFirstRangeThatInterruptsCoversThoseBeforeIt)
{
    const std::vector<Range> inflight = {{1, true}, {2, true}, {5, false}, {6, false}};
    EXPECT_EQ(raw::completionTarget(inflight, true, false), std::optional<size_t>(2));
}

TEST(CompletionTarget, QuietRangesWaitForTheSubmitThreadUntilItPromisesNothing)
{
    const std::vector<Range> inflight = {{1, true}, {2, true}};
    EXPECT_FALSE(raw::completionTarget(inflight, true, false));
    EXPECT_EQ(raw::completionTarget(inflight, false, false), std::optional<size_t>(1));
    EXPECT_EQ(raw::completionTarget(inflight, true, true), std::optional<size_t>(1)) << "waitUntilCompleted";
    EXPECT_FALSE(raw::completionTarget(std::vector<Range>{}, false, true));
}

// One device's channel, its completion thread and the kernel's quiet job ends, as a frame runs: each job is submitted
// (the completion thread looks at what is in flight after every range), then the GPU runs the frame.
struct Device {
    struct Job {
        uint32_t seqnos = 1;
        bool events = false;  // a shared event job: the kernel never takes it quiet
    };

    struct Buffer {
        std::vector<std::vector<Job>> ranges;  // each range's jobs, one raw commit each
        bool observed = false;                 // completed handlers or a present
    };

    explicit Device(bool quietOn) : quietOn(quietOn)
    {
        switch_gpu_quiet_init(&quiet, 0);
        switch_gpu_jobs_init(&jobs, 0);
    }

    void frame(const std::vector<Buffer> &buffers)
    {
        for (size_t b = 0; b < buffers.size(); ++b)
            for (size_t r = 0; r < buffers[b].ranges.size(); ++r) {
                const bool last = r + 1 == buffers[b].ranges.size();
                raw::RangeEnd end;
                end.lastOfBuffer = last;
                end.nextRangeRaw = !last;
                end.followed = b + 1 < buffers.size();
                end.observed = buffers[b].observed;
                commit(buffers[b].ranges[r], quietOn && raw::quietEnd(end));
                completionThread();
            }
        promised = false;
        completionThread();
        run();
    }

    // raw::CommandBuffer::commit: every job but the last quiet, the last as its range ends
    void commit(const std::vector<Job> &commitJobs, bool quietEnd)
    {
        for (size_t i = 0; i < commitJobs.size(); ++i) {
            const bool closing = i + 1 == commitJobs.size();
            const bool asked = quietOn && (!closing || quietEnd) && !commitJobs[i].events;
            const int announce = switch_gpu_quiet_announces(&quiet, asked);
            const uint32_t first = submitted + 1;
            submitted += commitJobs[i].seqnos;
            switch_gpu_jobs_kick(&jobs, first, submitted, 0, !announce);
            if (announce)
                switch_gpu_quiet_announce(&quiet, submitted);
            fifo.push_back({submitted, announce != 0});
        }
        inflight.push_back({submitted, quietOn && quietEnd});
        promised = quietOn && quietEnd;
    }

    // completionLoop: sleeps on its target unless asleep already; a quiet target asks the kernel (WaitFence)
    void completionThread()
    {
        if (sleepingOn)
            return;
        const auto target = raw::completionTarget(inflight, promised, false);
        if (!target)
            return;
        sleepingOn = inflight[*target].seqno;
        ++sleeps;
        if (switch_gpu_quiet_need(&quiet, sleepingOn) && switch_gpu_quiet_wake_due(&quiet, submitted)) {
            switch_gpu_quiet_announce(&quiet, submitted);
            switch_gpu_jobs_announce_newest(&jobs);
            fifo.push_back({0, true});
            ++wakeEntries;
        }
    }

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
            if (sleepingOn && int32_t(fence - sleepingOn) >= 0) {
                ++wakes;
                sleepingOn = 0;
                while (!inflight.empty() && int32_t(fence - inflight.front().seqno) >= 0)
                    inflight.pop_front();
                completionThread();
            }
        }
        EXPECT_TRUE(inflight.empty());
    }

    bool quietOn;
    switch_gpu_quiet quiet{};
    switch_gpu_jobs jobs{};
    uint32_t submitted = 0, fence = 0, sleepingOn = 0;
    std::deque<std::pair<uint32_t, bool>> fifo;
    std::deque<Range> inflight;
    bool promised = false;
    unsigned interrupts = 0, wakes = 0, sleeps = 0, wakeEntries = 0;
};

struct PerFrame {
    unsigned interrupts, wakes;
};

PerFrame measure(bool quietOn, const std::vector<Device::Buffer> &frame)
{
    Device d(quietOn);
    d.frame(frame);
    EXPECT_EQ(d.sleeps, d.wakes);
    return {d.interrupts, d.wakes};
}
}  // namespace

// The compositor's frame on T-v2: one command buffer, one job, completed handlers. Nothing to coalesce.
TEST(CompletionSourceModel, TheCompositorsOneJobFrameIsUnchanged)
{
    const std::vector<Device::Buffer> frame = {{{{{28, false}}}, true}};
    const PerFrame before = measure(false, frame), after = measure(true, frame);
    EXPECT_EQ(before.interrupts, 1u);
    EXPECT_EQ(after.interrupts, 1u);
    EXPECT_EQ(after.wakes, 1u);
}

// A frame of several jobs: two offscreen command buffers without handlers (the first split in two jobs for its push
// count), then the one that presents with a completed handler, submitted as two raw ranges.
TEST(CompletionSourceModel, AFrameOfSeveralJobsInterruptsOnceAndWakesTheCompletionThreadOnce)
{
    const std::vector<Device::Buffer> frame = {
        {{{{6, false}, {3, false}}}, false},
        {{{{4, false}}}, false},
        {{{{9, false}}, {{1, false}}}, true},
    };
    const PerFrame before = measure(false, frame), after = measure(true, frame);
    EXPECT_EQ(before.interrupts, 5u);
    EXPECT_EQ(before.wakes, 4u);
    EXPECT_EQ(after.interrupts, 1u);
    EXPECT_EQ(after.wakes, 1u);
}

// An event job always interrupts: its signal's waiters are woken by it.
TEST(CompletionSourceModel, AnEventJobKeepsItsInterrupt)
{
    const std::vector<Device::Buffer> frame = {{{{{1, true}, {5, false}}}, true}};
    const PerFrame after = measure(true, frame);
    EXPECT_EQ(after.interrupts, 2u);
    EXPECT_EQ(after.wakes, 1u);
}

// A quiet range with nothing submitted behind it (the next command buffer blocked): the submit thread's idle closes its
// promise and the completion thread's wait gets one wake entry.
TEST(CompletionSourceModel, AQuietRangeLeftBehindGetsOneWakeEntry)
{
    Device d(true);
    d.commit({{2, false}}, true);
    d.completionThread();
    EXPECT_EQ(d.sleeps, 0u);
    d.promised = false;
    d.completionThread();
    d.run();
    EXPECT_EQ(d.wakeEntries, 1u);
    EXPECT_EQ(d.interrupts, 1u);
    EXPECT_EQ(d.wakes, 1u);
}
