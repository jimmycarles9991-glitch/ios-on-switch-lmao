// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchSourceReadyDisplay.h: a swap whose surface a writer holds at its offer waits for that writer on a GPU ready
// watch, never on a sleeping thread, and its stop never outlives the watch's one delivery.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "../../TegraVIC/SwitchNativeScanout.h"

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnAborted = -14;
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object)               \
            (object)->release();  \
        (object) = nullptr;       \
    } while (0)
#define IOLog(...)           (++logs)
#define THREAD_UNINT         0
#define THREAD_AWAKENED      0
#define THREAD_CONTINUE_NULL nullptr

namespace {
unsigned logs, pitchCalls, blocks;
std::vector<int> pitchAnswers;
int observeError;
bool armedAtBlock;

class OSObject {
public:
    virtual ~OSObject() = default;
};

class IOInterruptEventSource {
public:
    using Action = void (*)(OSObject *, IOInterruptEventSource *, int);
    OSObject *owner = nullptr;
    Action action = nullptr;
    unsigned fires = 0, released = 0;

    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action)
    {
        auto *source = new IOInterruptEventSource;
        source->owner = owner;
        source->action = action;
        return source;
    }

    void interruptOccurred(void *, void *, int)
    {
        ++fires;
    }

    void release()
    {
        ++released;
    }

    void run()
    {
        action(owner, this, 0);
    }
};

struct IOWorkLoop {
    std::vector<IOInterruptEventSource *> sources;

    IOReturn addEventSource(IOInterruptEventSource *source)
    {
        sources.push_back(source);
        return kIOReturnSuccess;
    }

    void removeEventSource(IOInterruptEventSource *source)
    {
        for (auto at = sources.begin(); at != sources.end(); ++at) {
            if (*at == source) {
                sources.erase(at);
                return;
            }
        }
    }

    int runAction(int (*action)(OSObject *, void *, void *, void *, void *), OSObject *object)
    {
        return action(object, nullptr, nullptr, nullptr, nullptr);
    }
};

struct switch_gpu_ready_watch {
    int (*query)(void *);
    void (*callback)(void *, int);
    void *context;
    bool finished, cancelled, checking;
    unsigned references;
};

std::vector<switch_gpu_ready_watch *> watches;
using switch_gpu_ready_query = int (*)(void *);
using switch_gpu_ready_callback = void (*)(void *, int);

int switch_gpu_ready_observe(switch_gpu_ready_query query, switch_gpu_ready_callback callback, void *context,
                             switch_gpu_ready_watch **out)
{
    if (observeError)
        return observeError;
    auto *watch = new switch_gpu_ready_watch{query, callback, context, false, false, false, 2};
    watches.push_back(watch);
    *out = watch;
    return 0;
}

// As SwitchGpuReady.cpp's work: each admission change runs the query once; a terminal answer delivers once.
void admissionChanged()
{
    for (auto *watch : watches) {
        if (watch->finished)
            continue;
        int result = watch->query(watch->context);
        if (watch->cancelled)
            result = -125;
        if (result) {
            watch->finished = true;
            watch->callback(watch->context, result == 1 ? 0 : result);
            --watch->references;
        }
    }
}

void switch_gpu_ready_cancel(switch_gpu_ready_watch **input)
{
    if (!*input)
        return;
    auto *watch = *input;
    *input = nullptr;
    const bool notify = !watch->finished && !watch->checking;
    watch->cancelled = true;
    if (notify) {
        watch->finished = true;
        watch->callback(watch->context, -125);
        --watch->references;
    }
    --watch->references;
}

int switch_native_scanout_pitch(OSObject *, SwitchNativeScanout *scanout)
{
    ++pitchCalls;
    const int answer = pitchAnswers.empty() ? 0 : pitchAnswers.front();
    if (!pitchAnswers.empty())
        pitchAnswers.erase(pitchAnswers.begin());
    if (!answer)
        scanout->lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
    return answer;
}

struct OriginalLegacySwapRequest {};

struct NativeLegacyDisplay;
NativeLegacyDisplay *blocked;

void *current_thread()
{
    return nullptr;
}

void assert_wait(void *, int)
{}

void clear_wait(void *, int)
{}

void thread_wakeup(void *)
{}

void thread_block(void *);

struct NativeLegacyDisplay : OSObject {
    struct SourceWait {
        const OriginalLegacySwapRequest *request;
        OSObject *surface;
        SwitchNativeScanout read;
        int status;
    };

    SourceWait storage = {};
    SourceWait *sourceWait = &storage;
    bool sourceWaitQueued = false, copyStopping = false;
    IOWorkLoop loop;
    IOInterruptEventSource copyThread;
    IOInterruptEventSource *copySource = &copyThread;
    std::vector<int> finished;
    std::function<void()> whileStopping;

    IOWorkLoop *getWorkLoop()
    {
        return &loop;
    }

    void sourceWaitFinished()
    {
        sourceWaitQueued = false;
        finished.push_back(sourceWait->status);
    }

#include "../SwitchSourceReadyDisplay.h"
};

// The stop's wait for the delivery: the test plays the watch's thread here, as the real one runs while the stop sleeps.
void thread_block(void *)
{
    ++blocks;
    armedAtBlock = blocked->sourceReadyArmed;
    if (blocked->whileStopping)
        blocked->whileStopping();
}

class SourceReady : public testing::Test {
protected:
    NativeLegacyDisplay display;
    OSObject surface;

    void SetUp() override
    {
        logs = pitchCalls = blocks = 0;
        observeError = 0;
        armedAtBlock = false;
        pitchAnswers.clear();
        for (auto *watch : watches)
            delete watch;
        watches.clear();
        blocked = &display;
        display.startSourceReady();
        ASSERT_NE(display.sourceReadySource, nullptr);
        display.sourceWait->surface = &surface;
        display.sourceWaitQueued = true;
    }
};

TEST_F(SourceReady, ABusyWriterIsWatchedNotSleptOn)
{
    pitchAnswers = {-16, -16, 0};
    display.watchSource();
    EXPECT_EQ(display.copyThread.fires, 0u);
    admissionChanged();
    admissionChanged();
    EXPECT_EQ(display.sourceReadySource->fires, 0u);
    EXPECT_TRUE(display.finished.empty());
    admissionChanged();
    EXPECT_EQ(pitchCalls, 3u);
    EXPECT_EQ(display.sourceReadySource->fires, 1u);
    EXPECT_FALSE(display.sourceReadyArmed);
    EXPECT_TRUE(display.finished.empty());
    display.sourceReadySource->run();
    ASSERT_EQ(display.finished.size(), 1u);
    EXPECT_EQ(display.finished[0], 0);
    EXPECT_NE(display.sourceWait->read.lease, nullptr);
    EXPECT_EQ(display.sourceReady, nullptr);
    EXPECT_EQ(watches[0]->references, 0u);
}

TEST_F(SourceReady, AStaleSnapshotIsRetriedAndAFailureDelivered)
{
    pitchAnswers = {SWITCH_GPU_BACKING_STALE, -22};
    display.watchSource();
    admissionChanged();
    EXPECT_EQ(display.sourceReadySource->fires, 0u);
    admissionChanged();
    display.sourceReadySource->run();
    ASSERT_EQ(display.finished.size(), 1u);
    EXPECT_EQ(display.finished[0], -22);
}

TEST_F(SourceReady, OneDeliveryRunsTheFinishOnce)
{
    display.watchSource();
    admissionChanged();
    display.sourceReadySource->run();
    display.sourceReadySource->run();
    admissionChanged();
    EXPECT_EQ(display.finished.size(), 1u);
}

TEST_F(SourceReady, NoWatchFallsBackToTheCopyThreadAndSaysSoOnce)
{
    observeError = -28;
    display.watchSource();
    display.watchSource();
    EXPECT_EQ(display.copyThread.fires, 2u);
    EXPECT_EQ(logs, 1u);
    EXPECT_FALSE(display.sourceReadyArmed);
}

TEST_F(SourceReady, StopCancelsAndWaitsForTheDeliveryBeforeTheSourceGoes)
{
    pitchAnswers = {-16};
    display.watchSource();
    admissionChanged();
    watches[0]->checking = true;  // the watch's thread is inside the query: the cancel cannot deliver itself
    display.copyStopping = true;
    auto *source = display.sourceReadySource;
    display.whileStopping = [&] {
        watches[0]->checking = false;
        admissionChanged();
    };
    display.stopSourceReady();
    EXPECT_EQ(blocks, 1u);
    EXPECT_TRUE(armedAtBlock);
    EXPECT_FALSE(display.sourceReadyArmed);
    EXPECT_EQ(source->fires, 1u);
    EXPECT_EQ(source->released, 1u);
    EXPECT_EQ(display.sourceReadySource, nullptr);
    EXPECT_TRUE(display.loop.sources.empty());
    EXPECT_EQ(display.storage.status, -125);
    delete source;
}

TEST_F(SourceReady, StopWithNothingArmedDoesNotWait)
{
    auto *source = display.sourceReadySource;
    display.stopSourceReady();
    EXPECT_EQ(blocks, 0u);
    EXPECT_EQ(display.sourceReadySource, nullptr);
    delete source;
}
}  // namespace
