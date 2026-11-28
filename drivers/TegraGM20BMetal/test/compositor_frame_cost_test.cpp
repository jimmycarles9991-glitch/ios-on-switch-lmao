// SPDX-License-Identifier: MIT
// What a compositor frame costs our driver's two threads as the render server records them (the device opened by
// metal_prepare, so Device::Role::RenderServer): Core Animation shaped command buffers (scale_frames.h) recorded at
// the render thread's pace, each committed and waited for scheduled as backboardd does before its swap, on a GPU that
// takes a set time a frame (timed_gpu.h) well under a period. Per shape: the recording thread's CPU, the commit,
// waitUntilScheduled and where it went (frame_cpu_counts.h), the commit to the first job, and the submit worker's
// encode per command buffer, per pass and per draw with its kernel job calls (metal_encode_rates). The numbers are
// printed and written to compositor_frame_cost.txt; the test checks only that every frame completed.
#include "scale_frames.h"
#include "timed_gpu.h"
#include "../TegraGM20BMetal.h"
#include "../frame_cpu_counts.h"

#import <IOSurface/IOSurface.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" int thread_selfcounts(int type, void *buf, size_t nbytes);

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFrames = 40, kWarm = 4;
constexpr auto kGpuPerFrame = std::chrono::microseconds(3000);

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
};

const Opened &renderServer()
{
    static const Opened made = [] {
        const std::string pack = harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm));
        const std::string metallib = harness::writeMetallib();
        metal_prepare(pack.c_str(), 0);
        id<MTLDevice> device = harness::open(pack);
        if (!device)
            return Opened{};
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
        id<MTLRenderPipelineState> state =
            [device newRenderPipelineStateWithDescriptor:harness::descriptor(library, MTLPixelFormatBGRA8Unorm)
                                                   error:&error];
        return Opened{device, state};
    }();
    return made;
}

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

double us(Clock::duration d)
{
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) / 1000.0;
}

// The calling thread's instructions retired (thread_selfcounts THSC_CPI): steadier than its time on a shared executor
uint64_t instructions()
{
    uint64_t words[2] = {};
    return thread_selfcounts(1, words, sizeof words) ? 0 : words[0];
}

double threadUs()
{
    return double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1000.0;
}

// `key`=<value> of the encode line, its first number
double field(const std::string &line, const char *key)
{
    const std::string k = std::string(" ") + key + "=";
    const size_t at = line.find(k);
    return at == std::string::npos ? -1 : strtod(line.c_str() + at + k.size(), nullptr);
}

// after "key=a/" the b of a/b
double fieldCalls(const std::string &line, const char *key)
{
    const std::string k = std::string(" ") + key + "=";
    const size_t at = line.find(k);
    if (at == std::string::npos)
        return -1;
    const size_t slash = line.find('/', at + k.size());
    const size_t space = line.find(' ', at + k.size());
    return slash != std::string::npos && slash < space ? strtod(line.c_str() + slash + 1, nullptr) : -1;
}

size_t promiseMakes()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return size_t(std::count_if(kernel_model::model.promiseCalls.begin(), kernel_model::model.promiseCalls.end(),
                                [](const auto &c) {
                                    return c.op == TegraGM20BPromiseMake;
                                }));
}

std::string encodeLine()
{
    char out[4096];
    const size_t n = metal_encode_rates(out, sizeof out);
    return std::string(out, n);
}

// Core Animation's framebuffer: a BGRA8 IOSurface its last pass of a frame draws into, one of three in turn
std::vector<id<MTLTexture>> framebuffers(id<MTLDevice> device, const scale_frames::Shape &shape)
{
    std::vector<id<MTLTexture>> made;
    for (int i = 0; i < 3; i++) {
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @(shape.width),
            (id)kIOSurfaceHeight : @(shape.height),
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfaceBytesPerRow : @(shape.width * 4),
            (id)kIOSurfaceAllocSize : @(shape.width * 4 * shape.height),
            (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
        });
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:shape.width
                                                                                     height:shape.height
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        made.push_back([device newTextureWithDescriptor:td iosurface:surface plane:0]);
        CFRelease(surface);
    }
    return made;
}

// `presenting`: as Core Animation records: the command buffer enqueued before its first encoder, its last pass into one
// of three framebuffer IOSurfaces
std::string measure(const scale_frames::Shape &shape, bool presenting)
{
    const Opened &o = renderServer();
    timed_gpu::Gpu gpu;
    id<MTLCommandQueue> queue = [o.device newCommandQueue];
    auto into = scale_frames::targets(o.device, shape);
    const auto framebuffer = framebuffers(o.device, shape);
    const auto pace = std::chrono::microseconds(6000 / std::max(shape.passes, 1));
    std::vector<double> record, commit, scheduled, firstJob;
    uint64_t fewest = UINT64_MAX;
    std::vector<id<MTLCommandBuffer>> inFlight;
    gm20b::FrameCpuCounts &fc = gm20b::frameCpuCounts();
    uint64_t w0 = 0, q0 = 0, p0 = 0, e0 = 0, h0 = 0, o0 = 0, made0 = 0, small0 = 0;
    size_t jobs0 = 0, promises0 = 0;
    auto next = Clock::now();
    for (int frame = 0; frame < kWarm + kFrames; frame++) {
        if (frame == kWarm) {
            for (id<MTLCommandBuffer> cb : inFlight)
                [cb waitUntilCompleted];
            inFlight.clear();
            gpu.setPerJob(std::chrono::duration_cast<std::chrono::microseconds>(
                kGpuPerFrame / int(std::max<size_t>(gpu.submitted() / kWarm, 1))));
            (void)encodeLine();
            w0 = fc.waits, q0 = fc.waitQueued, p0 = fc.waitParked, e0 = fc.waitEncoding, h0 = fc.waitHandlers,
            o0 = fc.waitOther;
            made0 = fc.early[size_t(gm20b::EarlyOutcome::Made)];
            small0 = fc.early[size_t(gm20b::EarlyOutcome::Small)];
            jobs0 = gpu.submitted();
            promises0 = promiseMakes();
        }
        if (inFlight.size() == 2) {
            [inFlight.front() waitUntilCompleted];
            inFlight.erase(inFlight.begin());
        }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (presenting)
            [cb enqueue];
        double recordUs = 0;
        uint64_t recordInstructions = 0;
        for (int p = 0; p < shape.passes; p++) {
            const double began = threadUs();
            const uint64_t counted = instructions();
            if (presenting && p == shape.passes - 1)
                scale_frames::pass(cb, o.state, {framebuffer[size_t(frame) % framebuffer.size()]}, shape, shape.targets,
                                   uint32_t(frame));
            else
                scale_frames::pass(cb, o.state, into, shape, p, uint32_t(frame));
            recordUs += threadUs() - began;
            recordInstructions += instructions() - counted;
            std::this_thread::sleep_for(pace);
        }
        const size_t before = gpu.submitted();
        const auto committing = Clock::now();
        [cb commit];
        const auto returned = Clock::now();
        [cb waitUntilScheduled];
        const auto done = Clock::now();
        for (int spin = 0; spin < 2000 && gpu.submitted() <= before; spin++)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (frame >= kWarm) {
            record.push_back(recordUs);
            fewest = std::min(fewest, recordInstructions);
            commit.push_back(us(returned - committing));
            scheduled.push_back(us(done - returned));
            firstJob.push_back(us(gpu.submittedAt(before) - committing));
        }
        inFlight.push_back(cb);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    for (id<MTLCommandBuffer> cb : inFlight) {
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }
    const std::string enc = encodeLine();
    const uint64_t made = fc.early[size_t(gm20b::EarlyOutcome::Made)] - made0,
                   small = fc.early[size_t(gm20b::EarlyOutcome::Small)] - small0;
    const double waits = double(std::max<uint64_t>(fc.waits - w0, 1));
    const double encodes = field(enc, "n"), perCb = encodes > 0 ? encodes / kFrames : 0;
    const double wall = field(enc, "wall") * perCb, pass = field(enc, "pass") * perCb,
                 draw = field(enc, "draw") * perCb, kern = field(enc, "kern") * perCb,
                 execs = fieldCalls(enc, "exec") * perCb;
    char line[1024];
    snprintf(line, sizeof line,
             "compositor%s %d passes %d draws: record p50 %.1f us (%.2f a pass, %.3f a draw), fewest %llu instructions "
             "(%llu a pass); commit p50 %.1f us; "
             "waitUntilScheduled p50 %.1f p90 %.1f us (queued %.0f parked %.0f encoding %.0f handlers %.0f other "
             "%.0f; early %llu, too small %llu); commit to first job p50 %.1f p90 %.1f us; jobs %.2f a frame; worker "
             "after commit %.1f us a "
             "frame (pass %.1f, draw %.1f, kernel %.1f in %.2f job calls)\n",
             presenting ? " presenting" : "", shape.passes, shape.draws, percentile(record, 0.5),
             percentile(record, 0.5) / shape.passes, percentile(record, 0.5) / std::max(shape.draws, 1),
             (unsigned long long)fewest, (unsigned long long)(fewest / uint64_t(shape.passes)), percentile(commit, 0.5),
             percentile(scheduled, 0.5), percentile(scheduled, 0.9), double(fc.waitQueued - q0) / waits,
             double(fc.waitParked - p0) / waits, double(fc.waitEncoding - e0) / waits,
             double(fc.waitHandlers - h0) / waits, double(fc.waitOther - o0) / waits, (unsigned long long)made,
             (unsigned long long)small, percentile(firstJob, 0.5), percentile(firstJob, 0.9),
             double(gpu.submitted() - jobs0) / kFrames, wall, pass, draw, kern, execs);
    // every frame of more passes than the floor leaves after its commit is scheduled before its encode (frame line
    // early=), and a presenting one promises its framebuffer's writes to the kernel to be
    if (shape.passes >= 3) {
        EXPECT_EQ(made, uint64_t(kFrames)) << line;
        if (presenting)
            EXPECT_EQ(promiseMakes() - promises0, size_t(kFrames)) << line;
    }
    return line;
}

TEST(GM20BCompositorFrameCost, ScaleFrames)
{
    ASSERT_TRUE(renderServer().device != nil);
    const char *at = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    std::ofstream report(std::string(at ? at : ".") + "/compositor_frame_cost.txt");
    const scale_frames::Shape shapes[] = {{1, 6, 1}, {3, 18, 2}, {9, 30, 4}, {15, 90, 4}};
    for (const bool presenting : {false, true})
        for (const scale_frames::Shape &shape : shapes) {
            const std::string line = measure(shape, presenting);
            report << line;
            printf("%s", line.c_str());
        }
}

}  // namespace
