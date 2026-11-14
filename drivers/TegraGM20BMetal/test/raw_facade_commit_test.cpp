// SPDX-License-Identifier: MIT
// Core Animation shaped command buffers through the raw facade over the kernel model, recorded at the render thread's
// pace (a pass every few hundred microseconds), so the submit worker encodes the ended encoders ahead of the commit
// (RawMetalPipeline.mm): the words each job carries, written to the test's outputs (words.txt) for a comparison with
// another build's; what was encoded ahead and what took it back; and, with a GPU that holds each job, the time from the
// commit to its return, to waitUntilScheduled's and to the first job's submission (commit_latency.txt).
#include "raw_job_words.h"
#include "raw_scale_frames.h"
#include "timed_gpu.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {

using namespace harness;
using Clock = std::chrono::steady_clock;

using scale_frames::opened;

double percentileOf(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

std::string outputs()
{
    const char *at = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    return at ? at : dir();
}

size_t jobCount()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.jobs.size();
}

// `shape` recorded on `queue` a pass every `pace`, each pass beside `beside` where there is one
id<MTLCommandBuffer> paced(id<MTLCommandQueue> queue, const std::vector<id<MTLTexture>> &into,
                           const scale_frames::Shape &shape, uint32_t frame, std::chrono::microseconds pace,
                           id<MTLTexture> beside = nil)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, frame, beside);
        std::this_thread::sleep_for(pace);
    }
    return cb;
}

// What the worker encoded of `cb` ahead of its commit (RMCommandBuffer switchRawPipelined); empty on a build without it
NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    const SEL selector = sel_registerName("switchRawPipelined");
    if (![(id)cb respondsToSelector:selector])
        return nil;
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, selector);
}

std::string path(id<MTLCommandBuffer> cb)
{
    return [[(id)cb performSelector:@selector(switchRawPath)] UTF8String];
}

TEST(RawFacadeCommit, ScaleShapedJobWords)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    // the last beside a memoryless colour 1
    const scale_frames::Shape shapes[] = {{1, 6, 1}, {15, 90, 4}, {12, 134, 3}, {7, 20, 2}, {15, 90, 4}};
    std::vector<std::vector<id<MTLTexture>>> into;
    for (const auto &shape : shapes)
        into.push_back(scale_frames::targets(opened().device, shape));
    const id<MTLTexture> beside = scale_frames::scratch(opened().device, shapes[4]);
    std::vector<job_words::Run> runs;
    const uint64_t mark = job_words::handleMark();
    for (uint32_t frame = 0; frame < 3; frame++)
        for (size_t s = 0; s < std::size(shapes); s++) {
            id<MTLCommandBuffer> cb =
                paced(queue, into[s], shapes[s], frame, std::chrono::microseconds(300), s == 4 ? beside : nil);
            runs.push_back(job_words::run(cb));
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
            EXPECT_EQ(path(cb), "raw");
            if (NSDictionary *p = pipelined(cb))
                printf("shape %zu frame %u: %s\n", s, frame,
                       [[p description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String);
        }
    const std::string words = job_words::words(runs, mark);
    std::ofstream(outputs() + "/words.txt") << words;
    printf("scale words: %zu bytes, hash %016llx\n", words.size(), (unsigned long long)job_words::fnv(words));
    EXPECT_FALSE(words.empty());
}

TEST(RawFacadeCommit, EncodersEndedBeforeTheCommitAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    NSDictionary *before = counters(opened().device);
    for (uint32_t frame = 0; frame < 4; frame++) {
        id<MTLCommandBuffer> cb = paced(queue, into, shape, frame, std::chrono::milliseconds(1));
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        EXPECT_EQ([p[@"encoders"] intValue], 15);
        // all but what was recorded since the last hand-off (kHandOff) and the two encoders held for the look-ahead
        EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_EQ([p[@"veto"] intValue], 0);
    }
    NSDictionary *after = counters(opened().device);
    const auto delta = [&](NSString *key) {
        return [after[key] longLongValue] - [before[key] longLongValue];
    };
    printf("hand-offs over 4 frames: %lld, %lld of them woke the worker\n", delta(@"pipeline_handoffs"),
           delta(@"pipeline_handoff_wakes"));
}

TEST(RawFacadeCommit, PassesBesideAMemorylessColourAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    const id<MTLTexture> beside = scale_frames::scratch(opened().device, shape);
    ASSERT_NE(beside, nil);
    for (uint32_t frame = 0; frame < 4; frame++) {
        id<MTLCommandBuffer> cb = paced(queue, into, shape, frame, std::chrono::milliseconds(1), beside);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        EXPECT_EQ(path(cb), "raw");
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_EQ([p[@"veto"] intValue], 0);
    }
}

// Pass 9 of 15 into a target whose format the state does not draw: that encoder goes to m2v, refused as it records,
// after the worker was handed (and may be encoding) the ones before it. The device comes back whole: the next command
// buffer is encoded ahead as any other.
TEST(RawFacadeCommit, AnEncoderRefusedAfterTheHandOffsGivesTheDeviceBack)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:shape.width
                                                                                 height:shape.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    const id<MTLTexture> other = [opened().device newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        if (p == 9)
            scale_frames::pass(cb, opened().state, {other}, {1, 6, 1}, 0, 0);
        else
            scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    [cb commit];
    [cb waitUntilCompleted];
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    printf("refused at pass 9: %s, path %s\n",
           [[p description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String, path(cb).c_str());
    EXPECT_NE([p[@"veto"] intValue] & int(VetoRefused), 0);
    EXPECT_EQ([p[@"pipelined"] intValue], 0);
    EXPECT_LE([p[@"abandons"] intValue], 1);
    id<MTLCommandBuffer> next = paced(queue, into, shape, 1, std::chrono::milliseconds(1));
    [next commit];
    [next waitUntilCompleted];
    ASSERT_EQ(next.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(next), "raw");
    NSDictionary *q = pipelined(next);
    ASSERT_NE(q, nil);
    EXPECT_GE([q[@"pipelined"] intValue], 10) << [[q description] UTF8String];
    EXPECT_EQ([q[@"abandons"] intValue], 0);
}

// Copy-engine launches in the model's jobs from `first` on: a fresh texture's fill is one (RawMetalFresh.mm writeFresh)
size_t copyLaunches(size_t first)
{
    constexpr uint32_t kCopy = 4, kLaunchDma = 0x300;
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    size_t launches = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++) {
        const auto &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           method = (h & 0x1fff) * 4;
            if (op == 4) {
                launches += subc == kCopy && method == kLaunchDma;
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); k++, i++)
                launches += subc == kCopy && (op == 1        ? method + 4 * k
                                              : op == 5 && k ? method + 4
                                                             : method) == kLaunchDma;
        }
    }
    return launches;
}

id<MTLTexture> newTarget()
{
    return scale_frames::targets(opened().device, {1, 1, 1})[0];
}

// A new target its first pass clears and stores whole, with no renderTargetWidth set: Metal renders over the attachment
// whole, so nothing writes its contents first
TEST(RawFacadeCommit, ANewTargetClearedWholeTakesNoFill)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const size_t first = jobCount();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    scale_frames::pass(cb, opened().state, {newTarget()}, {1, 6, 1}, 0, 0);
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(copyLaunches(first), 0u);
}

// Each frame draws into two new targets, as the render server's close animations do: one cleared whole (no fill), one
// loaded first (its contents written ahead of the pass that loads it, in a blit of their own)
TEST(RawFacadeCommit, NewTargetsEachFrameAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    for (uint32_t frame = 0; frame < 4; frame++) {
        const size_t first = jobCount();
        const id<MTLTexture> cleared = newTarget(), loaded = newTarget();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int p = 0; p < shape.passes; p++) {
            if (p == 2)
                scale_frames::pass(cb, opened().state, {cleared}, {1, 6, 1}, 0, frame);
            else if (p == 5)
                scale_frames::pass(cb, opened().state, {loaded}, {1, 6, 1}, 1, frame);
            else
                scale_frames::pass(cb, opened().state, into, shape, p, frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        printf("two new targets, frame %u: %d of %d encoders encoded ahead, %zu copy launches\n", frame,
               [p[@"pipelined"] intValue], [p[@"encoders"] intValue], copyLaunches(first));
        EXPECT_GE([p[@"pipelined"] intValue], [p[@"encoders"] intValue] - 5) << [[p description] UTF8String];
        EXPECT_EQ([p[@"abandons"] intValue], 0);
        EXPECT_GE(copyLaunches(first), 1u);
    }
}

TEST(RawFacadeCommit, AnotherQueuesSubmissionTakesTheDeviceBack)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> recorded = [opened().device newCommandQueue], other = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4}, small{1, 6, 1};
    const auto into = scale_frames::targets(opened().device, shape);
    const auto elsewhere = scale_frames::targets(opened().device, small);
    id<MTLCommandBuffer> cb = [recorded commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (p == 9) {
            id<MTLCommandBuffer> between = scale_frames::record(other, opened().state, elsewhere, small, 0);
            [between commit];
            [between waitUntilCompleted];
            EXPECT_EQ(between.status, MTLCommandBufferStatusCompleted);
        }
    }
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(cb), "raw");
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"abandons"] intValue], 1) << [[p description] UTF8String];
    EXPECT_EQ([p[@"pipelined"] intValue], 0);
}

TEST(RawFacadeCommit, ACommandBufferWithAComputeEncoderIsEncodedAtTheCommit)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        if (p == 11)
            [[cb computeCommandEncoder] endEncoding];
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"pipelined"] intValue], 0) << [[p description] UTF8String];
}

struct Latency {
    std::vector<double> commit, scheduled, firstSubmit;
};

double us(Clock::duration d)
{
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) / 1000.0;
}

// Frames of `shape` at 60 Hz with the GPU taking `gpuUs` for each frame's jobs and up to three frames in flight: each
// recorded at the render thread's pace, committed, waited for scheduled as Core Animation does before its swap
Latency frames(const scale_frames::Shape &shape, int gpuUs, int count)
{
    timed_gpu::Gpu gpu;
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto into = scale_frames::targets(opened().device, shape);
    const auto pace = std::chrono::microseconds(6000 / std::max(shape.passes, 1));
    {
        id<MTLCommandBuffer> warm = scale_frames::record(queue, opened().state, into, shape, 0);
        [warm commit];
        [warm waitUntilCompleted];
        gpu.setPerJob(std::chrono::microseconds(gpuUs / int(std::max<size_t>(gpu.submitted(), 1))));
    }
    std::vector<id<MTLCommandBuffer>> inFlight;
    Latency out;
    auto next = Clock::now();
    for (int frame = 0; frame < count; frame++) {
        if (inFlight.size() == 3) {
            [inFlight.front() waitUntilCompleted];
            inFlight.erase(inFlight.begin());
        }
        id<MTLCommandBuffer> cb = paced(queue, into, shape, uint32_t(frame), pace);
        const size_t before = gpu.submitted();
        const auto committing = Clock::now();
        [cb commit];
        const auto returned = Clock::now();
        [cb waitUntilScheduled];
        const auto scheduled = Clock::now();
        for (int spin = 0; spin < 2000 && gpu.submitted() <= before; spin++)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        out.commit.push_back(us(returned - committing));
        out.scheduled.push_back(us(scheduled - returned));
        out.firstSubmit.push_back(us(gpu.submittedAt(before) - committing));
        inFlight.push_back(cb);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    for (id<MTLCommandBuffer> cb : inFlight)
        [cb waitUntilCompleted];
    return out;
}

uint64_t threadNs()
{
    return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}

// The caller's CPU recording a 15-pass, 90-draw command buffer (its encoders, not its commit), as many after it as
// before, the worker encoding ahead beside it: what the tape's publication and its growth cost the recording thread
TEST(RawFacadeCommit, RecordingCost)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    std::vector<double> recordUs, commitUs;
    for (uint32_t frame = 0; frame < 60; frame++) {
        const uint64_t began = threadNs();
        id<MTLCommandBuffer> cb = scale_frames::record(queue, opened().state, into, shape, frame);
        const uint64_t recorded = threadNs();
        [cb commit];
        const uint64_t committed = threadNs();
        [cb waitUntilCompleted];
        if (frame < 20)
            continue;
        recordUs.push_back(double(recorded - began) / 1000.0);
        commitUs.push_back(double(committed - recorded) / 1000.0);
    }
    char line[200];
    snprintf(line, sizeof line, "recording 15 passes 90 draws: thread cpu p50 %.1f p90 %.1f us, commit p50 %.1f us\n",
             percentileOf(recordUs, 0.5), percentileOf(recordUs, 0.9), percentileOf(commitUs, 0.5));
    std::ofstream(outputs() + "/recording_cost.txt") << line;
    printf("%s", line);
}

TEST(RawFacadeCommit, CommitLatency)
{
    ASSERT_TRUE(opened().device != nil);
    std::ofstream report(outputs() + "/commit_latency.txt");
    const scale_frames::Shape shapes[] = {{1, 6, 1}, {15, 90, 4}};
    for (const auto &shape : shapes)
        for (const int gpuUs : {2000, 16000}) {
            const Latency l = frames(shape, gpuUs, 24);
            char line[320];
            snprintf(line, sizeof line,
                     "passes %d draws %d gpu %d us: commit p50 %.1f p90 %.1f us, waitUntilScheduled p50 %.1f p90 %.1f "
                     "us, commit to first job p50 %.1f p90 %.1f us\n",
                     shape.passes, shape.draws, gpuUs, percentileOf(l.commit, 0.5), percentileOf(l.commit, 0.9),
                     percentileOf(l.scheduled, 0.5), percentileOf(l.scheduled, 0.9), percentileOf(l.firstSubmit, 0.5),
                     percentileOf(l.firstSubmit, 0.9));
            report << line;
            printf("%s", line);
        }
}

}  // namespace
