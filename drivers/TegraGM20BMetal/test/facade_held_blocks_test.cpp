// SPDX-License-Identifier: MIT
// What a committed, not yet completed command buffer keeps allocated through the facade over the kernel model, shaped
// as hw_test_app's gpu.path.encode_cost records it (encode_cost_shape.h): the blocks held over 24 command buffers
// committed one after another and held, per command buffer, each arm after a batch of its shape completed. Each held
// block is named by its allocation site and the code that made it (held_sites.h), with the timed GPU running the jobs as
// they come and held (nothing completes). The runtime's blocks a command buffer holds may not grow with its draws and
// stay within kRuntimeHeldMost; its allocations over a batch may not grow with the draws, nor its completion allocate.
#include "encode_cost_shape.h"
#include "held_sites.h"
#include "timed_gpu.h"

#include <gtest/gtest.h>
#include <time.h>
#include <cmath>

namespace held_blocks_test {

using encode_cost::kBuffers;

constexpr int kDraws[2] = {8, 16};
constexpr size_t kSiteFrames = 8;
constexpr double kRuntimeHeldMost = 2, kHeldMost = 4;

struct Arm {
    double misses = 0;
    held_sites::Window window;
    std::map<std::string, unsigned> paths;
};

double profileCount(NSDictionary<NSString *, NSNumber *> *before, NSDictionary<NSString *, NSNumber *> *after,
                    NSString *key)
{
    return after[key].doubleValue - before[key].doubleValue;
}

// 24 at `draws` held after 24 of them completed and were destroyed (whose recordings the held ones take)
Arm heldArm(timed_gpu::Gpu &gpu, int draws, bool gpuHeld)
{
    Arm arm;
    gpu.hold(true);
    EXPECT_TRUE(encode_cost::settle(draws, kBuffers, [&] {
        gpu.hold(false);
    }));
    @autoreleasepool {
        NSMutableArray<id<MTLCommandBuffer>> *held = [[NSMutableArray alloc] initWithCapacity:kBuffers];
        if (gpuHeld)
            gpu.hold(true);
        NSDictionary<NSString *, NSNumber *> *profiled = encode_cost::profile();
        held_sites_start();
        for (int i = 0; i < kBuffers; i++)
            [held addObject:encode_cost::commitOne(draws)];
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        arm.misses = profileCount(profiled, encode_cost::profile(), @"recording_misses") / kBuffers;
        arm.window = held_sites::collect(log, events, overflow, kSiteFrames);
        if (gpuHeld)
            gpu.hold(false);
        arm.paths = encode_cost::finish(held);
    }
    return arm;
}

// gpu.path.encode_cost's own sequence: every count's batch once, then per count a batch and the held 24, as its Swift
// task records them (no autorelease pool of their own but the held command buffers'). Per count, per held command buffer:
// the blocks held and the allocations made on every thread, the kernel model's (the kernel's on a device) left out, and
// the command buffers that found no kept recording
struct Sequence {
    double held[2] = {}, allocations[2] = {}, misses[2] = {};
    held_sites::Window allocated[2];
};

Sequence hwTestSequence()
{
    Sequence out;
    @autoreleasepool {
        for (int draws : kDraws)
            encode_cost::finish(encode_cost::batchUnpooled(draws));
        for (int a = 0; a < 2; a++) {
            encode_cost::finish(encode_cost::batchUnpooled(kDraws[a]));
            NSDictionary<NSString *, NSNumber *> *before = encode_cost::profile();
            NSMutableArray<id<MTLCommandBuffer>> *held = [[NSMutableArray alloc] initWithCapacity:kBuffers];
            held_sites_start();
            for (int i = 0; i < kBuffers; i++)
                [held addObject:encode_cost::commitOne(kDraws[a])];
            size_t events = 0;
            bool overflow = false;
            const held_sites_event *log = held_sites_stop(&events, &overflow);
            out.misses[a] = profileCount(before, encode_cost::profile(), @"recording_misses") / kBuffers;
            const held_sites::Window w = held_sites::collect(log, events, overflow, kSiteFrames);
            held_sites::Window all = held_sites::collect(log, events, overflow, kSiteFrames, true);
            out.held[a] = double(w.live - w.earlierFreed - w.held(held_sites::Owner::Model)) / kBuffers;
            out.allocations[a] = double(all.held(held_sites::Owner::Runtime)) / kBuffers;
            out.allocated[a] = std::move(all);
            encode_cost::finish(held);
        }
    }
    return out;
}

constexpr const char *kCompletionThread = "com.brunostjohn.switch.gm20b.completion";

struct Completions {
    long runtime = 0;  // the runtime's allocations on its completion thread
    held_sites::Window all;
};

// Every allocation over kBuffers command buffers at `draws`, each waited for before the next is committed: each one's
// completion leaves nothing in flight. One waited for first keeps the idle release away.
Completions idlingCompletions(int draws)
{
    Completions out;
    @autoreleasepool {
        [encode_cost::commitOne(draws) waitUntilCompleted];
        NSMutableArray<id<MTLCommandBuffer>> *done = [[NSMutableArray alloc] initWithCapacity:kBuffers];
        held_sites_start();
        for (int i = 0; i < kBuffers; i++) {
            id<MTLCommandBuffer> cb = encode_cost::commitOne(draws);
            [cb waitUntilCompleted];
            [done addObject:cb];
        }
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        out.all = held_sites::collect(log, events, overflow, kSiteFrames, true);
        EXPECT_FALSE(out.all.overflow);
        for (const held_sites::Site &s : out.all.sites) {
            const auto on = s.threads.find(kCompletionThread);
            if (s.owner == held_sites::Owner::Runtime && on != s.threads.end())
                out.runtime += on->second;
        }
        encode_cost::finish(done);
    }
    return out;
}

constexpr const char *kReleaseQueue = "queue com.brunostjohn.switch.gpu-memory";
constexpr NSUInteger kFramesMost = 160;

// As a process drawing frames: each command buffer at `draws` waited for and followed by a display period of idle GPU,
// for longer than the idle release's second, which only a second with no completion reaches. Every allocation after a
// quarter second of frames (a release an earlier deadline started is over by then).
held_sites::Window framesPastTheIdleSecond(int draws)
{
    held_sites::Window all;
    @autoreleasepool {
        NSMutableArray<id<MTLCommandBuffer>> *done = [[NSMutableArray alloc] initWithCapacity:kFramesMost];
        const auto frame = [&] {
            id<MTLCommandBuffer> cb = encode_cost::commitOne(draws);
            [cb waitUntilCompleted];
            [done addObject:cb];
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        const auto logged = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < logged)
            frame();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
        held_sites_start();
        while (std::chrono::steady_clock::now() < until && done.count < kFramesMost)
            frame();
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        all = held_sites::collect(log, events, overflow, kSiteFrames, true);
        EXPECT_FALSE(all.overflow);
        encode_cost::finish(done);
    }
    return all;
}

}  // namespace held_blocks_test

using namespace held_blocks_test;

TEST(GM20BFacadeHeldBlocks, ACommittedCommandBufferHoldsNoBlockOfTheRuntimePerDraw)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    ASSERT_NE(encode_cost::profile()[@"recording_misses"], nil) << "no recording counts in the encode profile";
    timed_gpu::Gpu gpu;
    for (int draws : kDraws)
        EXPECT_TRUE(encode_cost::settle(draws));
    for (const bool gpuHeld : {false, true}) {
        double runtime[2] = {};
        for (int a = 0; a < 2; a++) {
            const Arm arm = heldArm(gpu, kDraws[a], gpuHeld);
            const held_sites::Window &w = arm.window;
            EXPECT_LE(arm.misses, 0.1) << kDraws[a] << " draws, gpu " << (gpuHeld ? "held" : "running");
            for (const auto &[path, n] : arm.paths)
                EXPECT_EQ(path, "gm20b") << n << " command buffers at " << kDraws[a] << " draws";
            EXPECT_FALSE(w.overflow);
            runtime[a] = double(w.held(held_sites::Owner::Runtime)) / kBuffers;
            EXPECT_LE(runtime[a], kRuntimeHeldMost) << "the runtime's blocks a command buffer holds at " << kDraws[a]
                                                    << " draws, gpu " << (gpuHeld ? "held" : "running");
        }
        EXPECT_LT(runtime[1] - runtime[0], 0.5) << "the runtime's held blocks grow with the draws: " << runtime[0]
                                                << " at " << kDraws[0] << ", " << runtime[1] << " at " << kDraws[1];
    }
}

TEST(GM20BFacadeHeldBlocks, TheHwTestsSequenceHoldsAndAllocatesNoMoreWithItsDraws)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    ASSERT_NE(encode_cost::profile()[@"recording_misses"], nil) << "no recording counts in the encode profile";
    timed_gpu::Gpu gpu;
    const Sequence s = hwTestSequence();
    for (int a = 0; a < 2; a++) {
        EXPECT_LE(s.held[a], kHeldMost) << kDraws[a] << " draws";
        EXPECT_LE(s.misses[a], 0.1) << kDraws[a] << " draws";
    }
    EXPECT_LT(s.held[1] - s.held[0], 0.5);
    EXPECT_LT(s.allocations[1] - s.allocations[0], 0.5) << held_sites::differences(
        s.allocated[0], s.allocated[1], held_sites::Owner::Runtime, std::to_string(kDraws[0]) + " draws",
        std::to_string(kDraws[1]) + " draws", kBuffers, "command buffer");
}

// The GPU going idle at every command buffer (a frame's on a device) moves the idle release's deadline without a timer
// change, which allocates
TEST(GM20BFacadeHeldBlocks, ACommandBufferThatIdlesTheGpuAllocatesNothingOnTheCompletionThread)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    timed_gpu::Gpu gpu;
    const Completions c = idlingCompletions(kDraws[1]);
    EXPECT_LT(c.runtime, kBuffers / 4) << held_sites::differences(
        held_sites::Window{}, c.all, held_sites::Owner::Runtime, "none", std::to_string(kDraws[1]) + " draws", kBuffers,
        "command buffer");
}

// The completion thread's wait for the next range carries the idle release's deadline: frames wake it anyway, and no
// timer runs on the release's queue while they come
TEST(GM20BFacadeHeldBlocks, DrawingFramesNeverWakesTheIdleRelease)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    timed_gpu::Gpu gpu;
    const held_sites::Window all = framesPastTheIdleSecond(kDraws[1]);
    long wakes = 0;
    for (const held_sites::Site &s : all.sites) {
        const auto queue = s.threads.find(kReleaseQueue), completion = s.threads.find(kCompletionThread);
        wakes += queue == s.threads.end() ? 0 : queue->second;
        if (s.owner == held_sites::Owner::Runtime && completion != s.threads.end())
            wakes += completion->second;
    }
    EXPECT_EQ(wakes, 0) << held_sites::differences(held_sites::Window{}, all, held_sites::Owner::Runtime, "none",
                                                   "frames", 1, "window");
}

// libc++'s map insert and vector growth run lambdas of their own: the code that called them owns the block, as the
// kernel model's admissions and the timed GPU's stamps are the model's
TEST(GM20BFacadeHeldBlocks, AStandardContainerTheTestGrowsIsTheTests)
{
    std::map<uint32_t, std::pair<uint64_t, std::vector<uint64_t>>> admitted;
    std::vector<timed_gpu::Clock::time_point> stamps;
    held_sites_start();
    admitted[1] = {7, {}};
    stamps.push_back(timed_gpu::Clock::now());
    size_t events = 0;
    bool overflow = false;
    const held_sites_event *log = held_sites_stop(&events, &overflow);
    const held_sites::Window all = held_sites::collect(log, events, overflow, kSiteFrames, true);
    long owned[4] = {};
    for (const held_sites::Site &s : all.sites) {
        const auto on = s.threads.find("encoding thread");
        owned[int(s.owner)] += on == s.threads.end() ? 0 : on->second;
    }
    EXPECT_EQ(owned[int(held_sites::Owner::Runtime)], 0);
    EXPECT_EQ(owned[int(held_sites::Owner::Test)], 2);
    EXPECT_EQ(admitted.size() + stamps.size(), 2u);
}

// The runtime's allocations on every thread over a batch, by site: the device's encode profile counts the encoding
// thread's, which here include the kernel model's records of each job (the kernel's own memory on a device)
TEST(GM20BFacadeHeldBlocks, ACommandBuffersAllocationsDoNotGrowWithItsDraws)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    timed_gpu::Gpu gpu;
    const auto settle = [&](int draws) {
        gpu.hold(true);
        return encode_cost::settle(draws, kBuffers, [&] {
            gpu.hold(false);
        });
    };
    double runtime[2] = {};
    held_sites::Window w[2];
    for (int a = 0; a < 2; a++) {
        EXPECT_TRUE(settle(kDraws[a]));
        EXPECT_TRUE(settle(kDraws[a]));
        held_sites_start();
        EXPECT_TRUE(settle(kDraws[a]));
        size_t events = 0;
        bool overflow = false;
        const held_sites_event *log = held_sites_stop(&events, &overflow);
        w[a] = held_sites::collect(log, events, overflow, kSiteFrames, true);
        runtime[a] = double(w[a].held(held_sites::Owner::Runtime)) / kBuffers;
    }
    EXPECT_LT(runtime[1] - runtime[0], 0.5)
        << held_sites::differences(w[0], w[1], held_sites::Owner::Runtime, std::to_string(kDraws[0]) + " draws",
                                   std::to_string(kDraws[1]) + " draws", kBuffers, "command buffer");
}

// Once nothing is in flight, the completion thread gives back at the last drain's deadline, never before it. Last: the
// release it starts runs on while a later test would count allocations.
TEST(GM20BFacadeHeldBlocks, TheCompletionThreadGivesBackAtTheLastDrainsDeadline)
{
    ASSERT_NE(scale_frames::opened().device, nil);
    timed_gpu::Gpu gpu;
    @autoreleasepool {
        [encode_cost::commitOne(kDraws[0]) waitUntilCompleted];
    }
    const uint64_t due = gRM->idle.due(0);
    ASSERT_NE(due, 0u) << "no deadline after a drain";
    const uint64_t until = due + 2 * uint64_t(GpuMemoryRelease::kIdleNs);
    while (gRM->idle.due(0) && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const uint64_t seen = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ASSERT_EQ(gRM->idle.due(0), 0u) << "nothing given back two seconds past the deadline";
    EXPECT_GE(seen, due);
}
