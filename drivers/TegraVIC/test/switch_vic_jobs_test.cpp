// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_vic_jobs_stubs.h"
#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchVICJobs.cpp"
#include "../TegraVICCancel.cpp"
#include "../../../kits/SwitchEngineGate/SwitchEngineGate.h"

struct switch_gpu_bridge {
    std::vector<switch_gpu_backing_token> tokens;
};

namespace {
std::vector<switch_gpu_backing_token> sourceTokens;
std::vector<switch_gpu_backing_token> promiseTokens;  // the uses' pending writer promises
std::vector<uint32_t> publishedWords;
uint32_t arena[SWITCH_HOST1X_ARENA_WORDS], reservedWords, submittedSyncpt, submittedThreshold;
uint32_t arenaShort, arenaWaits;
uint32_t begins, aborts, commits, retained, bridges, released, roomWaits, submits, powerUses, powerDones;
int bridgeError, submitError, retainError, statusError, powerError, semanticResult = 1, batchSettled = 1;
switch_gpu_event_waits *preparedBatch;
std::vector<switch_host1x_fence *> fences;
std::vector<uint32_t> fenceBounds, watchedBounds;
std::map<std::pair<uint32_t, uint64_t>, int> producerPhysical;  // by engine and value; absent: ran

struct Notification {
    void (*completed)(void *, int);
    void *context;
};

std::vector<Notification> notifications;
uint64_t nextTestSequence = 0;
bool transactionLocked, changeProducer, busyOnce;
volatile uint64_t *changeGeneration;

// The engine and host1x behind the queue: the job syncpoint's value, what the cancellation did to the channel and the
// engine, in order, and an idle gate whose power-on proof moves the syncpoint by two as V2 does.
uint32_t hardware;
std::vector<std::string> engineSteps;
bool engineBoots = true;
IOLock *powerLock;

struct TestGatePlatform {
    bool start(void (*)(void *), void *)
    {
        return true;
    }

    void lock()
    {
        IOLockLock(powerLock);
    }

    void unlock()
    {
        IOLockUnlock(powerLock);
    }

    void sleep(void *)
    {
        throw std::runtime_error("the gate sleeps");
    }

    void wake(void *)
    {}

    void arm(uint32_t)
    {}

    uint64_t nowUs()
    {
        return 0;
    }

    void log(const char *line)
    {
        switch_test_log().emplace_back(line);
    }
};

SwitchEngineGate<TestGatePlatform> *testGate;
}

extern "C" {
int switch_gpu_backing_register_engine(uint32_t, const switch_gpu_backing_engine *)
{
    return 0;
}

int switch_gpu_backing_register_filter_status(uint32_t, switch_gpu_backing_filter_status)
{
    return 0;
}

void switch_gpu_backing_completed()
{}

int switch_gpu_backing_token_status(const switch_gpu_backing_token *)
{
    return 1;
}

int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *)
{
    return statusError;
}

int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int external)
{
    EXPECT_EQ(external, 0);
    const auto at = producerPhysical.find({token->engine, token->value});
    return at == producerPhysical.end() ? 1 : at->second;
}

int switch_gpu_backing_token_retain(const switch_gpu_backing_token *)
{
    EXPECT_TRUE(transactionLocked);
    if (retainError && retained == 1)
        return retainError;
    ++retained;
    return 0;
}

void switch_gpu_backing_token_release(const switch_gpu_backing_token *)
{
    EXPECT_GT(retained, 0u);
    --retained;
}

int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
                                     const switch_gpu_backing_guard *guards, uint32_t guardCount,
                                     const switch_gpu_backing_token *completion,
                                     switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(switch_test_queue_locked);
    EXPECT_FALSE(transactionLocked);
    ++begins;
    for (uint32_t i = 0; i < guardCount; ++i)
        if (*guards[i].generation != guards[i].expected)
            return SWITCH_GPU_BACKING_STALE;
    *transaction = {};
    transaction->uses = uses;
    transaction->count = count;
    transaction->locked = 1;
    transaction->completion = *completion;
    transactionLocked = true;
    return 0;
}

int switch_gpu_backing_begin_conversion_checked(switch_gpu_backing_use *uses, uint32_t count,
                                                switch_gpu_cpu_lease *const *, uint32_t,
                                                const switch_gpu_backing_guard *guards, uint32_t guardCount,
                                                const switch_gpu_backing_token *completion,
                                                switch_gpu_backing_transaction *transaction)
{
    return switch_gpu_backing_begin_checked(uses, count, guards, guardCount, completion, transaction);
}

int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *, switch_gpu_backing_token *out,
                                    uint32_t capacity, uint32_t *needed)
{
    EXPECT_TRUE(transactionLocked);
    *needed = static_cast<uint32_t>(sourceTokens.size());
    if (capacity < *needed)
        return -28;
    std::copy(sourceTokens.begin(), sourceTokens.end(), out);
    return 0;
}

int switch_gpu_backing_promise_dependencies(switch_gpu_backing_transaction *transaction, switch_gpu_backing_token *out,
                                            uint32_t capacity, uint32_t *needed, uint32_t ownFirst, uint32_t ownEnd)
{
    EXPECT_TRUE(transactionLocked);
    EXPECT_EQ(ownFirst, ownEnd);
    transaction->promised = 1;
    const uint32_t at = *needed;
    *needed += static_cast<uint32_t>(promiseTokens.size());
    if (capacity < *needed)
        return -28;
    std::copy(promiseTokens.begin(), promiseTokens.end(), out + at);
    return 0;
}

void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(transactionLocked);
    transactionLocked = false;
    transaction->locked = 0;
    ++aborts;
}

void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(transactionLocked);
    transactionLocked = false;
    transaction->locked = 0;
    ++commits;
}

int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count, switch_gpu_bridge **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    EXPECT_EQ(retained, count);
    if (busyOnce) {
        busyOnce = false;
        return -16;
    }
    if (bridgeError)
        return bridgeError;
    *out = new switch_gpu_bridge{{tokens, tokens + count}};
    ++bridges;
    if (changeGeneration) {
        __atomic_add_fetch(changeGeneration, 1, __ATOMIC_RELEASE);
        changeGeneration = nullptr;
    }
    if (changeProducer) {
        ++sourceTokens[0].value;
        changeProducer = false;
    }
    return 0;
}

void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *, uint32_t *id, uint32_t *threshold)
{
    *id = 170;
    *threshold = 0x1000001;
}

int switch_gpu_bridge_covers(const switch_gpu_bridge *bridge, const switch_gpu_backing_token *token)
{
    for (const auto &held : bridge->tokens)
        if (held.queue == token->queue && held.reset_generation == token->reset_generation &&
            held.value >= token->value)
            return 1;
    return 0;
}

int switch_gpu_bridge_status(const switch_gpu_bridge *)
{
    return 1;
}

int switch_gpu_bridge_notify(const switch_gpu_bridge *, void (*completed)(void *, int), void *context)
{
    EXPECT_FALSE(switch_test_queue_locked);
    notifications.push_back({completed, context});
    return 0;
}

void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                              void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t boundUs)
{
    EXPECT_TRUE(switch_test_queue_locked);
    fence->state = SWITCH_HOST1X_FENCE_QUEUED;
    fences.push_back(fence);
    fenceBounds.push_back(boundUs);
    return 0;
}

void switch_host1x_fence_watch(switch_host1x_fence *, uint32_t boundUs)
{
    watchedBounds.push_back(boundUs);
}

void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    if (*bridge) {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        delete *bridge;
        *bridge = nullptr;
        ++released;
    }
}

int switch_gpu_bridge_wait_room()
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    ++roomWaits;
    return 0;
}

int switch_host1x_job_reserve(uint32_t channel, uint32_t capacity, uint32_t **words)
{
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_VIC));
    EXPECT_TRUE(switch_test_queue_locked);
    EXPECT_EQ(reservedWords, 0u) << "one reservation at a time";
    if (arenaShort) {
        --arenaShort;
        return TEGRA_HOST1X_EAGAIN;
    }
    reservedWords = capacity;
    *words = arena;
    return 0;
}

int switch_host1x_job_wait_room(uint32_t, uint32_t)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    ++arenaWaits;
    return 0;
}

void switch_host1x_job_cancel(uint32_t)
{
    reservedWords = 0;
}

// The stream host1x gathers is the one the job path wrote into the arena.
int switch_host1x_job_submit(uint32_t, const uint32_t *first, uint32_t count, uint32_t syncpt, uint32_t threshold)
{
    ++submits;
    EXPECT_EQ(first, arena);
    EXPECT_LE(count, reservedWords);
    reservedWords = 0;
    publishedWords.assign(first, first + count);
    submittedSyncpt = syncpt;
    submittedThreshold = threshold;
    return submitError;
}

int tegra_host1x_syncpt_wait(uint32_t, uint32_t, uint32_t)
{
    throw std::runtime_error("unexpected producer wait");
}

void tegra_vic_describe(char *, size_t)
{}

int switch_vic_power_use()
{
    ++powerUses;
    return testGate ? testGate->use() : powerError;
}

void switch_vic_power_done()
{
    ++powerDones;
    if (testGate)
        testGate->done();
}

void switch_vic_power_idle()
{
    if (testGate)
        testGate->idle();
}

void tegra_host1x_describe_channel(uint32_t, char *buffer, size_t size)
{
    if (size)
        snprintf(buffer, size, "ch1 test");
}

uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    EXPECT_EQ(id, gSyncpt);
    return hardware;
}

int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t maxIncrements)
{
    EXPECT_EQ(id, gSyncpt);
    const auto distance = static_cast<int32_t>(threshold - hardware);
    if (distance > static_cast<int32_t>(maxIncrements) || maxIncrements > 2)
        return -22;
    if (distance > 0)
        hardware = threshold;
    engineSteps.emplace_back("release " + std::to_string(threshold));
    return 0;
}

void tegra_host1x_channel_teardown(uint32_t channel)
{
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_VIC));
    engineSteps.emplace_back("teardown");
}

void tegra_host1x_channel_restart(uint32_t channel)
{
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_VIC));
    engineSteps.emplace_back("restart");
}
}

IOLock *switch_vic_power_lock()
{
    return powerLock;
}

bool switch_vic_engine_powered()
{
    EXPECT_TRUE(powerLock->locked);
    return true;
}

void switch_vic_engine_describe(char *text, size_t size)
{
    snprintf(text, size, "falcon CPUCTL 0");
}

bool switch_vic_engine_reset(bool *drained)
{
    EXPECT_TRUE(powerLock->locked);
    *drained = true;
    engineSteps.emplace_back("reset");
    return true;
}

bool switch_vic_engine_boot()
{
    EXPECT_TRUE(powerLock->locked);
    engineSteps.emplace_back("boot");
    if (engineBoots)
        hardware += 2;
    return engineBoots;
}

const char *switch_vic_engine_wedge()
{
    engineSteps.emplace_back("wedge");
    return "falcon did not answer";
}

bool switch_vic_semantics_start(uint32_t, void (*)(switch_gpu_event_waits *))
{
    return true;
}

bool switch_vic_semantics_pending()
{
    return false;
}

int switch_vic_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchVICSemantic **out)
{
    *out = nullptr;
    return 0;
}

void switch_vic_semantics_commit(SwitchVICSemantic *)
{}

void switch_vic_semantics_physical(uint64_t)
{}

switch_gpu_event_waits *switch_vic_events_batch(SwitchVICEvents *)
{
    return nullptr;
}

uint32_t switch_vic_events_tokens(SwitchVICEvents *, const switch_gpu_backing_token **out)
{
    *out = nullptr;
    return 0;
}

int switch_vic_events_bind(SwitchVICEvents *, const switch_gpu_backing_token *)
{
    return 0;
}

void switch_vic_events_commit(SwitchVICEvents *)
{}

extern "C" {
void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{}

void switch_gpu_event_waits_release(switch_gpu_event_waits **)
{}

void switch_gpu_event_waits_abort(switch_gpu_event_waits **)
{}

void switch_gpu_event_waits_commit(switch_gpu_event_waits **)
{}

int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *)
{
    return semanticResult;
}

int switch_gpu_event_waits_settled_status(const switch_gpu_event_waits *)
{
    return batchSettled;
}

int switch_gpu_event_waits_covers_token(switch_gpu_event_waits *, const switch_gpu_backing_token *)
{
    return 1;
}

int switch_gpu_event_waits_prepare_tokens(const switch_gpu_backing_token *, uint32_t, switch_gpu_event_waits **)
{
    return 0;
}

int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *, uint32_t, switch_gpu_event_waits **out)
{
    *out = preparedBatch;
    return 0;
}

int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *, switch_gpu_event_waits *)
{
    return 0;
}
}

namespace {
class SwitchVICJobsTest : public testing::Test {
protected:
    uint8_t configurations[kSwitchVICConfigBytes] = {};
    TegraVICJob jobs[3] = {};
    switch_gpu_backing_use uses[1] = {{reinterpret_cast<switch_gpu_backing *>(1), SWITCH_GPU_BACKING_WRITE, 0}};

    void SetUp() override
    {
        begins = aborts = commits = retained = bridges = released = roomWaits = submits = powerUses = powerDones = 0;
        reservedWords = submittedSyncpt = submittedThreshold = arenaShort = arenaWaits = 0;
        bridgeError = submitError = retainError = statusError = powerError = 0;
        semanticResult = batchSettled = 1;
        preparedBatch = nullptr;
        fences.clear();
        fenceBounds.clear();
        watchedBounds.clear();
        producerPhysical.clear();
        notifications.clear();
        transactionLocked = changeProducer = busyOnce = false;
        changeGeneration = nullptr;
        sourceTokens.clear();
        promiseTokens.clear();
        publishedWords.clear();
        gQueue = {};
        nextTestSequence += 4096;
        gQueue.submitted = gQueue.completed = nextTestSequence;
        gCompleted = nextTestSequence;
        gFailed = gSuspended = gCancelling = gRecovered = gRecoveredReady = false;
        hardware = static_cast<uint32_t>(nextTestSequence);
        engineSteps.clear();
        switch_test_log().clear();
        engineBoots = true;
        testGate = nullptr;
        powerLock = IOLockAlloc();
        ASSERT_TRUE(switch_vic_cancel_start(161));
        for (auto &stage : Stages::stages)
            stage = {};
        gLock = IOLockAlloc();
        switch_test_queue_lock = gLock;
        gSyncpt = 161;
        ASSERT_TRUE(Results::start(gSyncpt));
        gConfigs = {0x90000000, sizeof(configurations), configurations};
        for (auto &job : jobs) {
            job.count = 1;
            job.method[0] = TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET;
        }
    }

    void TearDown() override
    {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        EXPECT_EQ(retained, 0u);
        for (auto &job : gQueue.jobs) {
            switch_gpu_bridge_release(&job.bridge);
            Results::cancel(&job.outcome);
        }
        EXPECT_EQ(bridges, released);
        EXPECT_EQ(reservedWords, 0u) << "every reservation submitted or cancelled";
        IOLockFree(gLock);
        gLock = nullptr;
        switch_test_queue_lock = nullptr;
        EXPECT_FALSE(powerLock->locked);
        IOLockFree(powerLock);
        delete gCancel;
        gCancel = nullptr;
    }

    int submit(uint32_t count = 1, const switch_gpu_backing_guard *guard = nullptr,
               void (*completion)(void *, int) = nullptr, void *context = nullptr)
    {
        const SwitchVICOwnership ownership = {uses, 1, guard, guard ? 1u : 0u, nullptr, 0};
        return switch_vic_submit_checked(jobs, count, nullptr, ownership, completion, context, nullptr);
    }

    // The threshold interrupt's delivery of a job's fence, on host1x's work loop.
    static void interrupt(size_t which)
    {
        auto *fence = fences.at(which);
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        fence->signaled(fence, 0);
    }

    // The fence's bound passed with its syncpoint short of it (host1x's bounds call, on its work loop).
    static void outlived(size_t which)
    {
        auto *fence = fences.at(which);
        ASSERT_EQ(fence->state, SWITCH_HOST1X_FENCE_QUEUED);
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        fence->signaled(fence, TEGRA_HOST1X_ETIMEDOUT);
    }

    // The threshold interrupt for every queued fence of the engine's syncpoint that it has reached.
    static void reachedInterrupts()
    {
        for (auto *fence : fences)
            if (fence->state == SWITCH_HOST1X_FENCE_QUEUED && fence->syncpt == gSyncpt &&
                SwitchVICQueue::reached(hardware, fence->threshold)) {
                fence->state = SWITCH_HOST1X_FENCE_IDLE;
                fence->signaled(fence, 0);
            }
    }

    static void runCancellation()
    {
        ASSERT_EQ(gCancel->entered, 1u);
        gCancel->entered = 0;
        gCancel->fire(gCancel->param, nullptr);
    }

    static void bridged(size_t which, int status)
    {
        const auto notification = notifications.at(which);
        notification.completed(notification.context, status);
    }

    void gpu(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i)
            sourceTokens.push_back({10, 1, SWITCH_GPU_ENGINE_GM20B, i + 1});
    }
};

TEST_F(SwitchVICJobsTest, PendingGpuProducerUsesHardwareBridgeOutsideOwnershipLocks)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(bridges, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    ASSERT_GE(publishedWords.size(), 3u);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
    EXPECT_EQ(publishedWords[1], 0x1000001u);
    EXPECT_EQ(publishedWords[2], 170u);
}

// A writer promise on what the job reads (its job not submitted yet) is acquired by the GPU-to-host1x bridge, as a pending
// GM20B producer is: no CPU sleep for it, and VIC's own wait is on the bridge's syncpoint.
TEST_F(SwitchVICJobsTest, AWriterPromiseIsAcquiredByTheBridge)
{
    promiseTokens = {{7, 0, SWITCH_GPU_ENGINE_PROMISE, 2052}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 1u);
    EXPECT_EQ(commits, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    ASSERT_EQ(gQueue.jobs[0].bridge->tokens.size(), 1u);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
    ASSERT_GE(publishedWords.size(), 3u);
    EXPECT_EQ(publishedWords[2], 170u);
}

TEST_F(SwitchVICJobsTest, ABatchIsGatheredFromWhereItWasWrittenWithItsLastJobsFence)
{
    ASSERT_EQ(submit(3), 0);
    EXPECT_EQ(submits, 1u);
    EXPECT_EQ(submittedSyncpt, gSyncpt);
    EXPECT_EQ(submittedThreshold, static_cast<uint32_t>(nextTestSequence + 3));
    EXPECT_EQ(publishedWords.size(), 3u * (2 + 3));
}

TEST_F(SwitchVICJobsTest, NoArenaRoomSleepsOutsideTheQueueAndOwnershipLocks)
{
    arenaShort = 1;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(arenaWaits, 1u);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(submits, 1u);
}

TEST_F(SwitchVICJobsTest, ExecutedGpuWriterStillBridgesUntilItsPixelsAreExternallyVisible)
{
    gpu();
    ASSERT_EQ(switch_gpu_backing_token_status(&sourceTokens[0]), 1);
    ASSERT_EQ(switch_gpu_backing_token_external_status(&sourceTokens[0]), 0);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 1u);
    EXPECT_EQ(commits, 1u);
}

TEST_F(SwitchVICJobsTest, PendingNvdecUsesFullWidthWaitWithoutGpuBridge)
{
    sourceTokens = {{0x181234567ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(begins, 1u);
    EXPECT_EQ(publishedWords[1], 0x81234567u);
    EXPECT_EQ(publishedWords[2], 162u);
}

TEST_F(SwitchVICJobsTest, MoreThanInlineDependenciesGrowAndPublishWithoutCpuProducerWait)
{
    gpu(65);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(aborts, 2u);
    EXPECT_EQ(commits, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens.size(), 65u);
}

TEST_F(SwitchVICJobsTest, TooManyDependenciesRefuseBeforeHardwarePublication)
{
    gpu(SWITCH_GPU_BRIDGE_MAX_TOKENS + 1);
    EXPECT_EQ(submit(), -28);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(bridges, 0u);
}

TEST_F(SwitchVICJobsTest, ChangedGenerationAfterBridgeSubmissionRefusesStaleRepresentation)
{
    gpu();
    volatile uint64_t generation = 1;
    changeGeneration = &generation;
    const switch_gpu_backing_guard guard = {uses[0].backing, &generation, 1};
    EXPECT_EQ(submit(1, &guard), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(released, 1u);
}

TEST_F(SwitchVICJobsTest, NewProducerDuringBridgeSetupRequiresReplacement)
{
    gpu();
    changeProducer = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 2u);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens[0].value, 11u);
}

TEST_F(SwitchVICJobsTest, BridgeRoomWaitRunsWithoutBackingOrQueueLocks)
{
    gpu();
    busyOnce = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(roomWaits, 1u);
    EXPECT_EQ(commits, 1u);
}

TEST_F(SwitchVICJobsTest, BridgeAndTokenFailuresUnwindBeforePublication)
{
    gpu(2);
    retainError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(bridges, 0u);
    retainError = 0;
    bridgeError = -19;
    EXPECT_EQ(submit(), -19);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(retained, 0u);
}

TEST_F(SwitchVICJobsTest, FailedHostSubmissionDropsBridgeAndAbortsOwnership)
{
    gpu();
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(gQueue.count, 0u);
}

TEST_F(SwitchVICJobsTest, BatchRetainsBridgeThroughItsFinalCompletion)
{
    gpu();
    ASSERT_EQ(submit(3), 0);
    EXPECT_EQ(gQueue.count, 3u);
    EXPECT_EQ(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[1].bridge, nullptr);
    EXPECT_NE(gQueue.jobs[2].bridge, nullptr);
}

TEST_F(SwitchVICJobsTest, FailedProducerDoesNotPublishConsumer)
{
    gpu();
    statusError = -125;
    EXPECT_EQ(submit(), -125);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
}
}  // namespace

TEST_F(SwitchVICJobsTest, AJobRetiresFromItsFenceOnTheWorkLoopWithNoThread)
{
    jobs[0].pixels = 8000;
    int heard = 1;
    const auto record = [](void *context, int status) {
        *static_cast<int *>(context) = status;
    };
    ASSERT_EQ(submit(1, nullptr, record, &heard), 0);
    ASSERT_EQ(fences.size(), 1u);
    EXPECT_EQ(fences[0]->syncpt, 161u);
    EXPECT_EQ(fences[0]->threshold, static_cast<uint32_t>(gQueue.submitted));
    EXPECT_EQ(fenceBounds[0], 2000000u + 1000u);
    EXPECT_TRUE(notifications.empty());
    EXPECT_EQ(heard, 1);
    const uint64_t fence = gQueue.submitted;
    interrupt(0);
    EXPECT_EQ(heard, 0);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(gQueue.completed, fence);
    EXPECT_EQ(waitSequence(fence), 0);
}

// A job whose stream waited for an NVDEC picture whose decode was then cancelled ran on memory NVDEC never wrote: the
// decode is recorded failed before NVDEC's syncpoint is released, and the job ends with that failure, its completion
// and its own hardware outcome both, so neither its frame nor a consumer on VIC's syncpoint takes the picture as good.
TEST_F(SwitchVICJobsTest, AJobThatRanOnACancelledDecodesPictureEndsWithTheDecodesFailure)
{
    const switch_gpu_backing_token decode = {0x181234567ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162};
    sourceTokens = {decode, {decode.value - 1, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    int heard = 1;
    const auto record = [](void *context, int status) {
        *static_cast<int *>(context) = status;
    };
    ASSERT_EQ(submit(1, nullptr, record, &heard), 0);
    const uint64_t placed = gQueue.submitted;
    const switch_gpu_backing_token token = {placed, switch_vic_result_state.generation, SWITCH_GPU_ENGINE_VIC, gSyncpt};
    ASSERT_EQ(fences.size(), 2u) << "the job's fence and the newest decode's";
    EXPECT_EQ(fences[1]->syncpt, 162u);
    EXPECT_EQ(fences[1]->threshold, 0x81234567u);
    producerPhysical[{SWITCH_GPU_ENGINE_NVDEC, decode.value}] = -5;
    interrupt(1);
    hardware = static_cast<uint32_t>(placed);
    EXPECT_EQ(switch_vic_token_filter_status(&token), 0) << "a host1x filter waiter sees the decode's failure";
    interrupt(0);
    EXPECT_EQ(heard, -5);
    EXPECT_EQ(Results::physicalStatus(&token), -5);
    EXPECT_EQ(waitSequence(placed), -5);

    sourceTokens = {{decode.value + 1, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    ASSERT_EQ(submit(1, nullptr, record, &heard), 0);
    interrupt(3);
    interrupt(2);
    EXPECT_EQ(heard, 0) << "a later decode that ran is not this job's failure";
}

// A job behind a stalled decode is held up upstream, not stalled: its bound, and that of the job queued behind it,
// starts only once the decode's fence comes in (here NVDEC's cancellation releasing it), so NVDEC's own bound decides
// and VIC is never reset for it. The jobs then run and fail with the decode's failure, and the one behind runs.
TEST_F(SwitchVICJobsTest, AnUpstreamStallNeverResetsVic)
{
    const switch_gpu_backing_token decode = {0x181234567ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162};
    sourceTokens = {decode};
    int placed = 1, behind = 1;
    const auto record = [](void *context, int status) {
        *static_cast<int *>(context) = status;
    };
    ASSERT_EQ(submit(1, nullptr, record, &placed), 0);
    sourceTokens.clear();
    ASSERT_EQ(submit(1, nullptr, record, &behind), 0);
    ASSERT_EQ(fences.size(), 3u);
    EXPECT_EQ(fenceBounds, (std::vector<uint32_t>{0, 0, 0})) << "no bound runs while the decode has not come in";
    EXPECT_TRUE(watchedBounds.empty());
    producerPhysical[{SWITCH_GPU_ENGINE_NVDEC, decode.value}] = -5;
    interrupt(1);
    EXPECT_EQ(watchedBounds.size(), 2u) << "both bounds start with the decode's released fence";
    hardware = static_cast<uint32_t>(gQueue.submitted);
    reachedInterrupts();
    EXPECT_EQ(placed, -5);
    EXPECT_EQ(behind, 0);
    EXPECT_EQ(gCancel->entered, 0u);
    EXPECT_TRUE(engineSteps.empty());
    EXPECT_EQ(gQueue.count, 0u);
}

// The same job whose decode came in and which then stops making progress is VIC's own stall: its bound runs from the
// decode's fence, and when it passes VIC is reset and the job fails.
TEST_F(SwitchVICJobsTest, AStallAfterTheInputsCameInStillResetsVic)
{
    sourceTokens = {{0x181234567ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    int heard = 1;
    ASSERT_EQ(submit(
                  1, nullptr,
                  [](void *context, int status) {
                      *static_cast<int *>(context) = status;
                  },
                  &heard),
              0);
    interrupt(1);
    ASSERT_EQ(watchedBounds.size(), 1u);
    outlived(0);
    EXPECT_EQ(heard, -5);
    runCancellation();
    EXPECT_EQ(engineSteps.front(), "teardown");
    EXPECT_NE(std::find(engineSteps.begin(), engineSteps.end(), "reset"), engineSteps.end());
}

// The bound times VIC alone: it starts when the GPU producer is done (D8), whichever of the two finishes first.
TEST_F(SwitchVICJobsTest, ABridgedJobEndsWhenItsFenceAndItsBridgeHaveBothFinishedInEitherOrder)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    ASSERT_EQ(fences.size(), 1u);
    EXPECT_EQ(fenceBounds[0], 0u);
    ASSERT_EQ(notifications.size(), 1u);
    interrupt(0);
    EXPECT_EQ(gQueue.count, 1u);
    EXPECT_EQ(released, 0u);
    bridged(0, 0);
    EXPECT_EQ(watchedBounds, std::vector<uint32_t>{2000000u + 2000000u});
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(released, 1u);

    sourceTokens.clear();
    gpu();
    ASSERT_EQ(submit(), 0);
    bridged(1, 0);
    EXPECT_EQ(gQueue.count, 1u);
    interrupt(1);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(waitSequence(gQueue.completed), 0);
}

TEST_F(SwitchVICJobsTest, CompletedHardwareWithFailedProducerRetiresOnlyItsOwnJob)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    const uint64_t failed = gQueue.submitted;
    bridged(0, -5);
    interrupt(0);
    EXPECT_EQ(waitSequence(failed), -5);
    sourceTokens.clear();
    ASSERT_EQ(submit(), 0);
    const uint64_t independent = gQueue.submitted;
    interrupt(1);
    EXPECT_EQ(waitSequence(independent), 0);
    EXPECT_EQ(waitSequence(failed), -5);
}

TEST_F(SwitchVICJobsTest, ABatchRetiresWholeOnItsLastJobsFence)
{
    jobs[0].pixels = jobs[1].pixels = jobs[2].pixels = 0;
    int heard = 1;
    ASSERT_EQ(submit(
                  3, nullptr,
                  [](void *context, int status) {
                      *static_cast<int *>(context) = status;
                  },
                  &heard),
              0);
    ASSERT_EQ(fences.size(), 1u);
    EXPECT_EQ(fences[0]->threshold, static_cast<uint32_t>(gQueue.submitted));
    EXPECT_EQ(fenceBounds[0], 3u * 2000000u);
    interrupt(0);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(heard, 0);
}

// The semantic outcome comes from the events' retirement after the physical result is published; the job ends there.
TEST_F(SwitchVICJobsTest, AJobWithASemanticBatchEndsOnItsSemanticCompletion)
{
    preparedBatch = reinterpret_cast<switch_gpu_event_waits *>(uintptr_t(0x50));
    int heard = 1;
    ASSERT_EQ(submit(
                  1, nullptr,
                  [](void *context, int status) {
                      *static_cast<int *>(context) = status;
                  },
                  &heard),
              0);
    const uint64_t fence = gQueue.submitted;
    interrupt(0);
    EXPECT_EQ(gQueue.count, 1u);
    EXPECT_EQ(heard, 1);
    semanticRetired(reinterpret_cast<switch_gpu_event_waits *>(uintptr_t(0x60)));
    EXPECT_EQ(gQueue.count, 1u);
    semanticResult = -125;
    semanticRetired(preparedBatch);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(heard, -125);
    EXPECT_EQ(waitSequence(fence), -125);
}

// The display's host1x filter waiter, with the syncpoint at the job's fence and the work loop not yet run: certain
// only once nothing but the fence is left (no bridge out, no semantic wait), and only for a job that did not fail.
// The display's host1x filter waiter, the work loop not yet run: certain only with the job's syncpoint at its fence,
// its bridge done, nothing failed and its events batch settled.
TEST_F(SwitchVICJobsTest, TheFilterStatusIsCertainOnlyOnceTheSyncpointIsAtTheFenceWithNothingElseLeft)
{
    const auto lastToken = [] {
        return switch_gpu_backing_token{gQueue.submitted, 1, SWITCH_GPU_ENGINE_VIC, gSyncpt};
    };
    ASSERT_EQ(submit(), 0);
    auto plain = lastToken();
    EXPECT_EQ(switch_vic_token_filter_status(&plain), 0) << "its syncpoint short of its fence";
    hardware = static_cast<uint32_t>(plain.value);
    EXPECT_EQ(switch_vic_token_filter_status(&plain), 1);
    auto other = plain;
    other.queue = gSyncpt + 1;
    EXPECT_EQ(switch_vic_token_filter_status(&other), 0);
    interrupt(0);
    EXPECT_EQ(switch_vic_token_filter_status(&plain), 0) << "retired: its slot is no longer the job's";

    gpu();
    ASSERT_EQ(submit(), 0);
    auto bridgedJob = lastToken();
    hardware = static_cast<uint32_t>(bridgedJob.value);
    EXPECT_EQ(switch_vic_token_filter_status(&bridgedJob), 0) << "its GPU producer's bridge still out";
    bridged(0, 0);
    EXPECT_EQ(switch_vic_token_filter_status(&bridgedJob), 1);
    interrupt(1);

    ASSERT_EQ(submit(), 0);
    auto failedJob = lastToken();
    hardware = static_cast<uint32_t>(failedJob.value);
    bridged(1, -5);
    EXPECT_EQ(switch_vic_token_filter_status(&failedJob), 0);
    interrupt(2);

    sourceTokens.clear();
    preparedBatch = reinterpret_cast<switch_gpu_event_waits *>(uintptr_t(0x50));
    batchSettled = 0;
    ASSERT_EQ(submit(), 0);
    auto semantic = lastToken();
    hardware = static_cast<uint32_t>(semantic.value);
    EXPECT_EQ(switch_vic_token_filter_status(&semantic), 0) << "its semantic wait still out";
    batchSettled = 1;
    EXPECT_EQ(switch_vic_token_filter_status(&semantic), 1);
    interrupt(3);
    semanticRetired(preparedBatch);
}

TEST_F(SwitchVICJobsTest, JobsRetireAndCompleteInSubmissionOrder)
{
    std::vector<int> order;
    const auto record = [](void *context, int) {
        auto *pair = static_cast<std::pair<std::vector<int> *, int> *>(context);
        pair->first->push_back(pair->second);
    };
    std::pair<std::vector<int> *, int> first{&order, 1}, second{&order, 2};
    gpu();
    ASSERT_EQ(submit(1, nullptr, record, &first), 0);
    sourceTokens.clear();
    ASSERT_EQ(submit(1, nullptr, record, &second), 0);
    interrupt(0);
    interrupt(1);
    EXPECT_TRUE(order.empty());
    EXPECT_EQ(gQueue.count, 2u);
    bridged(0, 0);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_EQ(gQueue.count, 0u);
}

// A job past its bound fails its frame, never a panic: the batch behind it fails too (its completion hears the
// failure, so the display does not show that frame), the channel is torn down and restarted around the engine's
// reset, their fences are released, and the next frame's job runs on the renumbered queue.
TEST_F(SwitchVICJobsTest, AStalledFrameIsCancelledAndTheNextFrameRuns)
{
    const auto heard = [](void *context, int status) {
        *static_cast<int *>(context) = status;
    };
    int stalledFrame = 1, behindFrame = 1, nextFrame = 1;
    ASSERT_EQ(submit(1, nullptr, heard, &stalledFrame), 0);
    const uint64_t stalled = gQueue.submitted;
    ASSERT_EQ(submit(2, nullptr, heard, &behindFrame), 0);
    const uint64_t behind = gQueue.submitted;
    outlived(0);
    EXPECT_EQ(stalledFrame, -5);
    EXPECT_EQ(waitSequence(stalled), -5);
    EXPECT_EQ(gQueue.count, 2u);
    EXPECT_THROW(submit(), std::runtime_error) << "new jobs wait for the cancellation";
    IOLockUnlock(gLock);
    runCancellation();
    EXPECT_EQ(engineSteps,
              (std::vector<std::string>{"teardown", "reset", "release " + std::to_string(uint32_t(behind) - 1),
                                        "release " + std::to_string(uint32_t(behind)), "restart", "boot"}))
        << "two increments at a time, through the cancelled batch's last job";
    EXPECT_EQ(behindFrame, 1) << "the cancelled batch ends on its released fence";
    reachedInterrupts();
    EXPECT_EQ(behindFrame, -5);
    EXPECT_EQ(waitSequence(behind), -5);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_FALSE(gCancelling);
    ASSERT_EQ(switch_test_log().size(), 1u);
    EXPECT_NE(switch_test_log()[0].find("TegraVIC: job fence " + std::to_string(stalled) + " not reached"),
              std::string::npos);
    EXPECT_NE(switch_test_log()[0].find("engine reset: running again"), std::string::npos);
    ASSERT_EQ(submit(1, nullptr, heard, &nextFrame), 0);
    EXPECT_EQ(gQueue.submitted, behind + 3) << "after the proof's two increments";
    hardware = static_cast<uint32_t>(gQueue.submitted);
    reachedInterrupts();
    EXPECT_EQ(nextFrame, 0);
    EXPECT_EQ(gQueue.count, 0u);
}

TEST_F(SwitchVICJobsTest, AnEngineTheResetDoesNotBringBackRefusesTheNextJob)
{
    ASSERT_EQ(submit(), 0);
    engineBoots = false;
    outlived(0);
    EXPECT_FALSE(switch_vic_jobs_park(false)) << "the idle gate leaves a cancelling engine powered";
    runCancellation();
    EXPECT_EQ(engineSteps.back(), "wedge");
    ASSERT_EQ(switch_test_log().size(), 1u);
    EXPECT_NE(switch_test_log()[0].find("it did not come back, VIC refuses work: falcon did not answer"),
              std::string::npos);
    EXPECT_EQ(submit(), -19);
}

// After an idle power-off (SwitchEngineGate expire), the next job powers VIC on: the power-on's proof moves the
// syncpoint by two, the queue rebases on it, and the job runs on the next fence.
TEST_F(SwitchVICJobsTest, AnIdleGatedEngineIsPoweredOnByTheNextJob)
{
    SwitchEngineGate<TestGatePlatform> gate;
    const SwitchEngineGateOps ops = {
        "TegraVIC",
        nullptr,
        [](void *) -> int {
            hardware += 2;
            switch_vic_jobs_resume(true, hardware);
            return 0;
        },
        [](void *, bool drain) -> int {
            return switch_vic_jobs_park(drain) ? SWITCH_ENGINE_GATE_OFF : SWITCH_ENGINE_GATE_BUSY;
        },
    };
    ASSERT_TRUE(gate.start(ops));
    testGate = &gate;
    gate.expire();
    EXPECT_FALSE(gate.powered());
    EXPECT_TRUE(gSuspended);
    const uint64_t base = gQueue.submitted;
    int frame = 1;
    EXPECT_EQ(submit(
                  1, nullptr,
                  [](void *context, int status) {
                      *static_cast<int *>(context) = status;
                  },
                  &frame),
              0);
    testGate = nullptr;
    EXPECT_TRUE(gate.powered());
    EXPECT_FALSE(gSuspended);
    EXPECT_EQ(gQueue.submitted, base + 3);
    hardware = static_cast<uint32_t>(gQueue.submitted);
    reachedInterrupts();
    EXPECT_EQ(frame, 0);
    EXPECT_EQ(gQueue.count, 0u);
}

// A submission records its output as a write, or VIC refuses it: an unrecorded write would leave the GPU's L2 lines of the
// allocation valid for its next reader.
TEST_F(SwitchVICJobsTest, ASubmissionWithoutAWriteUseIsRefused)
{
    switch_gpu_backing_use readOnly[1] = {{uses[0].backing, SWITCH_GPU_BACKING_READ, 0}};
    EXPECT_EQ(
        switch_vic_submit_checked(jobs, 1, nullptr, {nullptr, 0, nullptr, 0, nullptr, 0}, nullptr, nullptr, nullptr),
        -22);
    EXPECT_EQ(
        switch_vic_submit_checked(jobs, 1, nullptr, {readOnly, 1, nullptr, 0, nullptr, 0}, nullptr, nullptr, nullptr),
        -22);
    EXPECT_EQ(gQueue.submitted, gQueue.completed);
}

// Idle power gating (TegraVIC.cpp, SwitchEngineGate.h): a submission holds the powered engine for the whole call, an
// idle power-off parks only an empty queue, and the power-on's proof stream moves the syncpoint the queue rebases on.
TEST_F(SwitchVICJobsTest, EverySubmissionHoldsThePowerOnceAndReleasesIt)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(powerUses, 1u);
    EXPECT_EQ(powerDones, 1u);
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(powerUses, 2u);
    EXPECT_EQ(powerDones, 2u);
    submitError = 0;
    powerError = -19;
    const uint32_t before = submits;
    EXPECT_EQ(submit(), -19);
    EXPECT_EQ(submits, before);
    EXPECT_EQ(powerDones, 2u);
}

TEST_F(SwitchVICJobsTest, AnIdlePowerOffParksOnlyAnEmptyQueue)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_FALSE(switch_vic_jobs_park(false));
    EXPECT_FALSE(gSuspended);
    interrupt(0);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_TRUE(switch_vic_jobs_park(false));
    EXPECT_TRUE(gSuspended);
}

TEST_F(SwitchVICJobsTest, AParkedQueueHoldsTheNextSubmissionUntilAResumeRebasesIt)
{
    ASSERT_TRUE(switch_vic_jobs_park(false));
    EXPECT_THROW(submit(), std::runtime_error) << "the submission sleeps until resume";
    IOLockUnlock(gLock);
    const uint64_t base = gQueue.submitted;
    switch_vic_jobs_resume(true, static_cast<uint32_t>(base) + 2);
    EXPECT_FALSE(gSuspended);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(gQueue.submitted, base + 3);
    EXPECT_EQ(powerUses, powerDones);
}
