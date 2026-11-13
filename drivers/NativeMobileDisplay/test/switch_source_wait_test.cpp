// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdlib>
#include <functional>
#include <vector>
#include "../../TegraVIC/SwitchNativeScanout.h"
using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -10, kIOReturnNoMemory = -12, kIOReturnAborted = -14;
constexpr int TEGRA_SCANOUT_SOURCE_LOST = -5;
#define IONewZero(type, count) static_cast<type *>(calloc(count, sizeof(type)))
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object)               \
            object->release();    \
        object = nullptr;         \
    } while (0)

class OSObject {
public:
    unsigned refs = 1;

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

struct OriginalLegacySwapRequest {};

namespace {
unsigned leases, waits, cpuWaits, observations, producerWaits;
int promisedNow;  // the surface's writer promises pending (SwitchGpuWriterPromise.h)
int waitError, producerError, producerVisible;
switch_gpu_backing_token producer;
std::function<void()> waiting;

uint64_t mach_absolute_time()
{
    return 100;
}

struct CacheOp {
    const volatile void *va;
    size_t bytes;
    unsigned afterWaits;
};

std::vector<CacheOp> cleanInvalidates, invalidates;

[[maybe_unused]] void switch_dcache_clean_invalidate(const volatile void *va, size_t bytes)
{
    cleanInvalidates.push_back({va, bytes, producerWaits});
}

[[maybe_unused]] void switch_dcache_invalidate(const volatile void *va, size_t bytes)
{
    invalidates.push_back({va, bytes, producerWaits});
}

void native_legacy_transfer_observed(int, const OriginalLegacySwapRequest *, uint64_t)
{
    ++observations;
}

struct Event {
    unsigned fires = 0;

    void interruptOccurred(void *, void *, int)
    {
        ++fires;
    }
};

struct Loop {
    int runAction(int (*action)(OSObject *, void *, void *, void *, void *), OSObject *object)
    {
        return action(object, nullptr, nullptr, nullptr, nullptr);
    }
};

struct NativeLegacyDisplay : OSObject {
    static constexpr int kNoBuffer = -1;
    static constexpr bool stats = true;
    bool copyStopping = false, copyQueued = false, transferred = false, latchRetire = true, planeNextSet = true;
    unsigned kicks = 0, lost = 0, taken = 0;
    Event event;
    Event *copySource = &event;
    Loop loop;
    const OriginalLegacySwapRequest *request = nullptr;
    std::function<void()> onKick;

    Loop *getWorkLoop()
    {
        return &loop;
    }

    const OriginalLegacySwapRequest *firstUntaken()
    {
        return request;
    }

    const OriginalLegacySwapRequest *pendingSwapRequest()
    {
        return request;
    }

    void kickSwapQueue()
    {
        ++kicks;
        if (onKick)
            onKick();
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {
        ++taken;
    }

    void dropLostFrame(const OriginalLegacySwapRequest *, int)
    {
        ++lost;
    }

    void watchSource()
    {
        copySource->interruptOccurred(nullptr, nullptr, 0);
    }

#include "../SwitchSourceWaitDisplay.h"

    ~NativeLegacyDisplay()
    {
        sourceWaitQueued = false;
        releaseSourceWait();
        free(sourceWait);
    }
};
}

int switch_native_scanout_pitch_wait(OSObject *, SwitchNativeScanout *out)
{
    ++waits;
    if (waiting)
        waiting();
    if (waitError)
        return waitError;
    out->lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
    out->view.producer = producer;
    ++leases;
    return 0;
}

// the CPU's own read: once the promised job is submitted, it names that job
int switch_native_scanout_pitch_cpu(OSObject *object, SwitchNativeScanout *out)
{
    ++cpuWaits;
    promisedNow = 0;
    return switch_native_scanout_pitch_wait(object, out);
}

extern "C" int switch_gpu_backing_promised(switch_gpu_backing *)
{
    return promisedNow;
}

// As SwitchGpuTimeline.cpp's external_wait: 1 once the writes are DRAM-visible, 0 still pending, negative failed.
extern "C" int switch_gpu_backing_token_external_wait(const switch_gpu_backing_token *token)
{
    EXPECT_EQ(token->value, producer.value);
    ++producerWaits;
    return producerError ? producerError : producerVisible;
}

void switch_native_scanout_release(SwitchNativeScanout *read)
{
    if (read->lease) {
        EXPECT_GT(leases, 0u);
        --leases;
    }
    *read = {};
}

namespace {
class SourceWait : public testing::Test {
protected:
    NativeLegacyDisplay *display;
    OriginalLegacySwapRequest request, other;
    OSObject surface;

    void SetUp() override
    {
        leases = waits = cpuWaits = observations = producerWaits = 0;
        waitError = producerError = promisedNow = 0;
        producerVisible = 1;
        cleanInvalidates.clear();
        invalidates.clear();
        producer = {};
        waiting = {};
        display = new NativeLegacyDisplay;
        display->request = &request;
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(leases, 0u);
        EXPECT_EQ(surface.refs, 1u);
    }

    void defer()
    {
        ASSERT_TRUE(display->deferSource(&request, &surface));
    }
};

TEST_F(SourceWait, HandoffDoesNotWaitInsideDisplayGateAndPinsTheRetry)
{
    defer();
    EXPECT_EQ(waits, 0u);
    EXPECT_EQ(display->event.fires, 1u);
    EXPECT_TRUE(display->transferred);
    display->onKick = [&] {
        EXPECT_EQ(leases, 1u);
        EXPECT_FALSE(display->transferred);
    };
    display->sourceWaitWork();
    EXPECT_EQ(waits, 1u);
    EXPECT_EQ(display->kicks, 1u);
    EXPECT_EQ(leases, 0u);
}

TEST_F(SourceWait, CpuFallbackKeepsSnapshotUntilTheActualCopyCompletes)
{
    defer();
    display->onKick = [&] {
        display->copyQueued = true;
    };
    display->sourceWaitWork();
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(surface.refs, 2u);
    display->copyQueued = false;
    display->releaseSourceWait();
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(surface.refs, 1u);
}

TEST_F(SourceWait, FailedProducerNeverRetriesOrCopiesItsPixels)
{
    defer();
    waitError = -5;
    display->sourceWaitWork();
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_EQ(display->lost, 1u);
    EXPECT_EQ(display->taken, 1u);
    EXPECT_FALSE(display->planeNextSet);
}

TEST_F(SourceWait, DiscardedRequestAndStopReleaseWithoutPresenting)
{
    defer();
    display->request = &other;
    display->sourceWaitWork();
    EXPECT_EQ(display->kicks, 0u);
    display->request = &request;
    defer();
    display->copyStopping = true;
    display->sourceWaitWork();
    EXPECT_EQ(waits, 1u);
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_EQ(leases, 0u);
}

TEST_F(SourceWait, RetryFailureDoesNotSpinOrFallThroughToCpuCopy)
{
    defer();
    waitError = -12;
    display->sourceWaitWork();
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_EQ(observations, 1u);
    EXPECT_EQ(display->event.fires, 1u);
    EXPECT_FALSE(display->transferred);
}

TEST_F(SourceWait, WorkerStorageIsReusedAndAnUnavailableWorkerRefusesTheFrame)
{
    defer();
    display->sourceWaitWork();
    auto *storage = display->sourceWait;
    defer();
    display->sourceWaitWork();
    EXPECT_EQ(storage, display->sourceWait);
    display->copySource = nullptr;
    defer();
    EXPECT_EQ(display->event.fires, 2u);
    EXPECT_EQ(waits, 2u);
    EXPECT_EQ(observations, 1u);
}
}

TEST_F(SourceWait, OrdinaryCpuFallbackTakesOwnershipBeforeItReadsAndReusesDeferredLease)
{
    ASSERT_TRUE(display->holdCopySource(&surface));
    EXPECT_EQ(waits, 0u);
    ASSERT_EQ(display->waitCopySource(), 0);
    EXPECT_EQ(waits, 1u);
    EXPECT_EQ(leases, 1u);
    ASSERT_TRUE(display->holdCopySource(&surface));
    ASSERT_EQ(display->waitCopySource(), 0);
    EXPECT_EQ(waits, 1u);
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(surface.refs, 2u);
}

// A deferral's read was taken behind a writer promise (an engine reader orders on it on the GPU): the CPU copy cannot,
// and takes its read again once the promised job is submitted, so the producer it waits for is that job.
TEST_F(SourceWait, CpuFallbackTakesAPromisedReadAgainAfterThePromisedJob)
{
    defer();
    display->onKick = [&] {
        display->copyQueued = true;
    };
    display->sourceWaitWork();
    ASSERT_EQ(waits, 1u);
    ASSERT_EQ(leases, 1u);
    promisedNow = 1;
    producer = {7, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    ASSERT_EQ(display->waitCopySource(), 0);
    EXPECT_EQ(cpuWaits, 1u);
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(producerWaits, 1u);
    display->copyQueued = false;
    display->releaseSourceWait();
}

TEST_F(SourceWait, OnlyCpuFallbackWaitsTheExactProducerAndRejectsItsFailure)
{
    producer = {42, 1, SWITCH_GPU_ENGINE_VIC, 3};
    defer();
    display->sourceWaitWork();
    EXPECT_EQ(producerWaits, 0u);
    ASSERT_TRUE(display->holdCopySource(&surface));
    producerError = -5;
    EXPECT_EQ(display->waitCopySource(), -5);
    EXPECT_EQ(producerWaits, 1u);
    EXPECT_EQ(leases, 1u);
}

TEST_F(SourceWait, CpuFallbackReadsAnEngineFrameOnceItsWritesAreVisible)
{
    producer = {42, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    ASSERT_TRUE(display->holdCopySource(&surface));
    EXPECT_EQ(display->waitCopySource(), 0);
    EXPECT_EQ(producerWaits, 1u);
    producerVisible = 0;
    EXPECT_NE(display->waitCopySource(), 0);
    EXPECT_EQ(producerWaits, 2u);
}

TEST_F(SourceWait, CpuCopyCleansAndInvalidatesItsReadOnlySourceOnlyAfterItsProducer)
{
    producer = {42, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    const char pixels[256] = {};
    uint64_t gpu = 0, cache = 0;
    ASSERT_TRUE(display->holdCopySource(&surface));
    ASSERT_EQ(display->readyCopySource(pixels, sizeof(pixels), gpu, cache), 0);
    ASSERT_EQ(cleanInvalidates.size(), 1u);
    EXPECT_EQ(cleanInvalidates[0].va, pixels);
    EXPECT_EQ(cleanInvalidates[0].bytes, sizeof(pixels));
    EXPECT_EQ(cleanInvalidates[0].afterWaits, 1u);
    EXPECT_TRUE(invalidates.empty());
    producerError = -5;
    EXPECT_EQ(display->readyCopySource(pixels, sizeof(pixels), gpu, cache), -5);
    EXPECT_EQ(cleanInvalidates.size(), 1u);
    EXPECT_TRUE(invalidates.empty());
}
