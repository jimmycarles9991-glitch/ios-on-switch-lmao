// SPDX-License-Identifier: MIT
// A private texture moved off compressed memory from another thread (rmTexturePlain, as an m2v twin or
// switchRawProbeMovePlain moves it) while the submit worker holds the device's one command buffer open for an encode
// ahead (RawMetalPipeline.mm), through the raw facade over the kernel model: the move's job and the command buffer encoded
// ahead must each reach the GPU as they were submitted. The GPU here takes a set time for each job and compares, as it
// completes one, the words its pushes hold then with those they held at its submission: words written over a job the
// GPU has not run yet are a corrupt job on hardware.
#include "raw_scale_frames.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

using scale_frames::opened;

// A GPU for the kernel model that runs each job for `perJob`, in submission order, and counts the jobs whose pushes'
// words changed between their submission and their completion
class CheckingGpu {
public:
    explicit CheckingGpu(std::chrono::microseconds perJob) : perJob_(perJob)
    {
        using kernel_model::model;
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        first_ = model.jobs.size();
        model.jobAdded = [this] {
            std::lock_guard<std::mutex> g(lock_);
            ++added_;
            wake_.notify_all();
        };
        model.blockUntil = [this](uint32_t channel, uint32_t seqno) {
            std::unique_lock<std::mutex> g(lock_);
            return wake_.wait_for(g, std::chrono::seconds(30), [&] {
                return int32_t(kernel_model::model.fence(channel - 1).completed - seqno) >= 0;
            });
        };
        thread_ = std::thread([this] {
            loop();
        });
    }

    ~CheckingGpu()
    {
        {
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait_for(g, std::chrono::seconds(10), [&] {
                return done_ == added_;
            });
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.jobAdded = nullptr;
        kernel_model::model.blockUntil = nullptr;
    }

    size_t changed()
    {
        std::lock_guard<std::mutex> g(lock_);
        return changed_;
    }

    size_t firstChanged()
    {
        std::lock_guard<std::mutex> g(lock_);
        return firstChanged_;
    }

private:
    void loop()
    {
        using kernel_model::model;
        for (;;) {
            size_t at;
            {
                std::unique_lock<std::mutex> g(lock_);
                wake_.wait(g, [&] {
                    return stopping_ || done_ < added_;
                });
                if (stopping_)
                    return;
                at = first_ + done_;
            }
            std::this_thread::sleep_for(perJob_);
            kernel_model::Job job;
            std::vector<uint32_t> now;
            {
                std::lock_guard<std::mutex> l(kernel_model::callLock);
                job = model.jobs[at];
                now = model.pushWords(job.pushes.data(), uint32_t(job.pushes.size()));
            }
            {
                std::lock_guard<std::mutex> g(lock_);
                if (!job.pushes.empty() && now != job.words && !changed_++)
                    firstChanged_ = at;
                __atomic_store_n(&model.fence(job.channel - 1).completed, job.seqno, __ATOMIC_RELEASE);
                ++done_;
            }
            wake_.notify_all();
        }
    }

    const std::chrono::microseconds perJob_;
    std::mutex lock_;
    std::condition_variable wake_;
    size_t first_ = 0, added_ = 0, done_ = 0, changed_ = 0, firstChanged_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

id probe(id<MTLDevice> device, const char *selector, id<MTLTexture> texture)
{
    return ((id (*)(id, SEL, id))objc_msgSend)((id)device, sel_registerName(selector), texture);
}

// A private BGRA8 target on compressed memory: the first of its size asks for the memory, a later one takes it
id<MTLTexture> compressedTarget(id<MTLDevice> device)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:256
                                                                                 height:256
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    for (int i = 0; i < 500; i++) {
        id<MTLTexture> t = [device newTextureWithDescriptor:td];
        if ([(NSString *)probe(device, "switchRawProbeFacts:", t) containsString:@"comptags=1"])
            return t;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nil;
}

NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    const SEL selector = sel_registerName("switchRawPipelined");
    if (![(id)cb respondsToSelector:selector])
        return nil;
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, selector);
}

TEST(RawFacadeMoveAhead, AMoveFromAnotherThreadLeavesEveryJobItMeetsAsSubmitted)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    id<MTLTexture> moved = compressedTarget(device);
    ASSERT_NE(moved, nil) << "no compressed memory was made";
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(device, shape);
    CheckingGpu gpu(std::chrono::milliseconds(3));
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    NSNumber *answer = nil;
    for (int p = 0; p < shape.passes; p++) {
        scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (p == 9) {
            std::thread other([&] {
                answer = probe(device, "switchRawProbeMovePlain:", moved);
            });
            other.join();
        }
    }
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_TRUE(answer.boolValue) << "the texture left compressed memory";
    NSDictionary *p = pipelined(cb);
    printf("encode ahead: %s\n", p ? [[p description] UTF8String] : "-");
    EXPECT_EQ(gpu.changed(), 0u) << "job " << gpu.firstChanged() << "'s pushes were written over before the GPU ran it";
}

}  // namespace
