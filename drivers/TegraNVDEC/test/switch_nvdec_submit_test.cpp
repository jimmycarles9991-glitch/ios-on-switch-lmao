// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvdec_submit_stubs.h"
#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchNVDECJobs.cpp"
#include "../TegraNVDECCancel.cpp"
#include "../../../kits/SwitchEngineGate/SwitchEngineGate.h"

struct switch_gpu_bridge {
    std::vector<switch_gpu_backing_token> tokens;
};

namespace {
std::vector<switch_gpu_backing_token> sourceTokens;
std::vector<switch_gpu_backing_token> promiseTokens;  // the uses' pending writer promises
std::vector<uint32_t> publishedWords;
uint32_t arena[SWITCH_HOST1X_ARENA_WORDS], reservedWords, submittedSyncpt, submittedThreshold;
uint32_t arenaShort, arenaWaits, cancels;
uint32_t begins, aborts, commits, retained, bridges, released, roomWaits, submits, powerUses, powerDones;
int bridgeError, submitError, retainError, statusError, powerError, semanticResult = 1, batchSettled = 1;
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
bool semanticCapture;
uint32_t preparedBatches;

struct FakeBatch {};

// The engine and host1x behind the queue: the job syncpoint's value, what the cancellation did to the channel and the
// engine, in order, and an idle gate whose power-on proof moves the syncpoint by two as N2 does.
uint32_t hardware;
std::vector<std::string> engineSteps;
bool enginePowered = true, engineBoots = true;
IOLock *powerLock;

struct TestGatePlatform {
    void (*fire)(void *) = nullptr;
    void *context = nullptr;
    uint32_t armed = 0;

    bool start(void (*callback)(void *), void *gate)
    {
        fire = callback;
        context = gate;
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
    {
        ++armed;
    }

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

int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count, const switch_gpu_backing_token *completion,
                             switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(switch_test_queue_locked);
    EXPECT_FALSE(transactionLocked);
    ++begins;
    *transaction = {};
    transaction->uses = uses;
    transaction->count = count;
    transaction->locked = 1;
    transaction->completion = *completion;
    transactionLocked = true;
    return 0;
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
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_DEFAULT));
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
    ++cancels;
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

bool tegra_nvdec_ready()
{
    return true;
}

int switch_gpu_backing_cpu_acquire(switch_gpu_backing *, uint64_t, uint64_t, uint32_t)
{
    return 0;
}

int switch_gpu_backing_cpu_publish(switch_gpu_backing *, uint64_t, uint64_t)
{
    return 0;
}

int switch_gpu_backing_cpu_release(switch_gpu_backing *, uint64_t, uint64_t, uint32_t)
{
    return 0;
}

void tegra_host1x_describe_channel(uint32_t, char *, size_t)
{}

void tegra_host1x_describe(char *buffer, size_t size)
{
    snprintf(buffer, size, "ch0 test");
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
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_DEFAULT));
    engineSteps.emplace_back("teardown");
}

void tegra_host1x_channel_restart(uint32_t channel)
{
    EXPECT_EQ(channel, uint32_t(TEGRA_HOST1X_CHANNEL_DEFAULT));
    engineSteps.emplace_back("restart");
}

IOLock *tegra_nvdec_power_lock(void)
{
    return powerLock;
}
}

bool switch_nvdec_engine_powered()
{
    EXPECT_TRUE(powerLock->locked);
    return enginePowered;
}

void switch_nvdec_engine_describe(char *text, size_t size)
{
    snprintf(text, size, "falcon CPUCTL 0 IDLESTATE 0x31");
}

bool switch_nvdec_engine_reset(bool *drained)
{
    EXPECT_TRUE(powerLock->locked);
    *drained = true;
    engineSteps.emplace_back("reset");
    return true;
}

bool switch_nvdec_engine_boot()
{
    EXPECT_TRUE(powerLock->locked);
    engineSteps.emplace_back("boot");
    if (engineBoots)
        hardware += 2;
    return engineBoots;
}

const char *switch_nvdec_engine_wedge()
{
    engineSteps.emplace_back("wedge");
    return "falcon did not answer";
}

[[noreturn]] void switch_nvdec_job_fault(uint32_t, int, uint64_t)
{
    throw std::runtime_error("decode fault");
}

int switch_nvdec_power_use()
{
    ++powerUses;
    return testGate ? testGate->use() : powerError;
}

void switch_nvdec_power_done()
{
    ++powerDones;
    if (testGate)
        testGate->done();
}

void switch_nvdec_power_idle()
{
    if (testGate)
        testGate->idle();
}

bool switch_nvdec_semantics_start(uint32_t, void (*)(switch_gpu_event_waits *))
{
    return true;
}

bool switch_nvdec_semantics_pending()
{
    return false;
}

int switch_nvdec_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchNVDECSemantic **out)
{
    *out = nullptr;
    return 0;
}

void switch_nvdec_semantics_commit(SwitchNVDECSemantic *)
{}

void switch_nvdec_semantics_physical(uint64_t)
{}

extern "C" {
void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{}

void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
    delete reinterpret_cast<FakeBatch *>(*batch);
    *batch = nullptr;
}

void switch_gpu_event_waits_abort(switch_gpu_event_waits **batch)
{
    switch_gpu_event_waits_release(batch);
}

void switch_gpu_event_waits_commit(switch_gpu_event_waits **batch)
{
    *batch = nullptr;
}

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
    return !semanticCapture || preparedBatches ? 1 : 0;
}

int switch_gpu_event_waits_prepare_tokens(const switch_gpu_backing_token *, uint32_t, switch_gpu_event_waits **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    *out = nullptr;
    ++preparedBatches;
    return 0;
}

int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *, uint32_t, switch_gpu_event_waits **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    *out = reinterpret_cast<switch_gpu_event_waits *>(new FakeBatch);
    return 0;
}

int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *, switch_gpu_event_waits *)
{
    return 0;
}
}

namespace {
class SwitchNVDECSubmitTest : public testing::Test {
protected:
    uint8_t setup[SwitchNVDECQueue::kSlots * kSetupStride] = {};
    Submission work = {};
    TegraNVDECSubmit request = {};
    uint32_t fence = 0;

    void SetUp() override
    {
        begins = aborts = commits = retained = bridges = released = roomWaits = submits = preparedBatches = 0;
        reservedWords = submittedSyncpt = submittedThreshold = arenaShort = arenaWaits = cancels = 0;
        powerUses = powerDones = 0;
        bridgeError = submitError = retainError = statusError = powerError = 0;
        semanticResult = batchSettled = 1;
        fences.clear();
        fenceBounds.clear();
        watchedBounds.clear();
        producerPhysical.clear();
        notifications.clear();
        for (auto &stage : Stages::stages)
            stage = {};
        transactionLocked = changeProducer = busyOnce = semanticCapture = false;
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
        enginePowered = engineBoots = true;
        testGate = nullptr;
        powerLock = IOLockAlloc();
        ASSERT_TRUE(switch_nvdec_cancel_start(162));
        gLock = IOLockAlloc();
        switch_test_queue_lock = gLock;
        gSyncpt = 162;
        ASSERT_TRUE(Results::start(gSyncpt));
        gSetup = {nullptr, setup, 0x90000000, sizeof(setup)};
        request.entry_count = 1;
        request.entries[0] = {NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 0, 0};
        request.setup_bytes = 4;
        work.uses[0] = {reinterpret_cast<switch_gpu_backing *>(1), SWITCH_GPU_BACKING_WRITE, 0};
        work.useCount = 1;
    }

    void TearDown() override
    {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        EXPECT_EQ(retained, 0u);
        for (auto &job : gQueue.jobs) {
            switch_gpu_bridge_release(&job.bridge);
            switch_gpu_event_waits_release(&job.semantics);
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

    int submit(const SwitchNVDECCompletion &completion = {})
    {
        waitedForRoom = false;
        return publish(request, work, completion, &fence, &waitedForRoom);
    }

    bool waitedForRoom = false;

    void gpu(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i)
            sourceTokens.push_back({10, 1, SWITCH_GPU_ENGINE_GM20B, i + 1});
    }

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
                SwitchNVDECQueue::reached(hardware, fence->threshold)) {
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

    // The events' retirement reporting the job's semantic outcome.
    static void semantic(uint64_t sequence)
    {
        switch_gpu_event_waits *batch = nullptr;
        for (const auto &job : gQueue.jobs)
            if (job.sequence == sequence)
                batch = job.semantics;
        ASSERT_NE(batch, nullptr);
        semanticRetired(batch);
    }
};

TEST_F(SwitchNVDECSubmitTest, PendingGpuUsesHardwareBridgeOutsideOwnershipLocks)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(fence, nextTestSequence + 1);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
    EXPECT_EQ(publishedWords[1], 0x1000001u);
    EXPECT_EQ(publishedWords[2], 170u);
}

// A writer promise on a reference picture (its job not submitted yet) is acquired by the GPU-to-host1x bridge, with no CPU
// sleep for it.
TEST_F(SwitchNVDECSubmitTest, AWriterPromiseIsAcquiredByTheBridge)
{
    promiseTokens = {{7, 0, SWITCH_GPU_ENGINE_PROMISE, 2052}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(commits, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    ASSERT_EQ(gQueue.jobs[0].bridge->tokens.size(), 1u);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
    EXPECT_EQ(publishedWords[2], 170u);
}

TEST_F(SwitchNVDECSubmitTest, AJobIsGatheredFromWhereItWasWrittenWithItsOwnFence)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(submits, 1u);
    EXPECT_EQ(submittedSyncpt, gSyncpt);
    EXPECT_EQ(submittedThreshold, fence);
    EXPECT_EQ(publishedWords.back(), tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt));
}

TEST_F(SwitchNVDECSubmitTest, NoArenaRoomSleepsOutsideTheQueueAndOwnershipLocks)
{
    arenaShort = 1;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(arenaWaits, 1u);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(submits, 1u);
}

TEST_F(SwitchNVDECSubmitTest, ExecutedGpuStillBridgesUntilExternalVisibility)
{
    gpu();
    ASSERT_EQ(switch_gpu_backing_token_status(&sourceTokens[0]), 1);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 1u);
}

TEST_F(SwitchNVDECSubmitTest, VicWaitKeepsFullLow32Threshold)
{
    sourceTokens = {{0x181234567ULL, 1, SWITCH_GPU_ENGINE_VIC, 161}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(publishedWords[1], 0x81234567u);
    EXPECT_EQ(publishedWords[2], 161u);
}

TEST_F(SwitchNVDECSubmitTest, OwnQueueDependencyUsesEngineOrderWithoutCpuWait)
{
    sourceTokens = {{10, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0));
}

TEST_F(SwitchNVDECSubmitTest, LargeDependencySetGrowsOutsideOwnershipAndPublishes)
{
    gpu(70);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(aborts, 2u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens.size(), 70u);
}

TEST_F(SwitchNVDECSubmitTest, LargeVicDependencySetHasEnoughCommandStorage)
{
    for (uint32_t i = 0; i < 100; ++i)
        sourceTokens.push_back({0x181234567ULL + i, 1, SWITCH_GPU_ENGINE_VIC, 161});
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(publishedWords.size(), 308u);
    EXPECT_EQ(publishedWords[298], 0x812345cau);
}

TEST_F(SwitchNVDECSubmitTest, TooManyDependenciesRejectBeforeHardware)
{
    gpu(SWITCH_GPU_BRIDGE_MAX_TOKENS + 1);
    EXPECT_EQ(submit(), -28);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
}

TEST_F(SwitchNVDECSubmitTest, ChangedProducerDuringBridgeSetupRequiresFreshBridge)
{
    gpu();
    changeProducer = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 2u);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(begins, 3u);
}

TEST_F(SwitchNVDECSubmitTest, BridgePressureSleepsOnlyOutsideOwnership)
{
    gpu();
    busyOnce = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(roomWaits, 1u);
}

TEST_F(SwitchNVDECSubmitTest, FailedProducerNeverPublishes)
{
    gpu();
    statusError = -125;
    EXPECT_EQ(submit(), -125);
    EXPECT_EQ(submits, 0u);
}

TEST_F(SwitchNVDECSubmitTest, FailedKickUnwindsBridgeAndOwnership)
{
    gpu();
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(released, 1u);
}

TEST_F(SwitchNVDECSubmitTest, SemanticCaptureDropsBackingAndQueueLocksBeforeAllocation)
{
    gpu();
    semanticCapture = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(preparedBatches, 1u);
    EXPECT_EQ(begins, 3u);
    EXPECT_NE(gQueue.jobs[0].semantics, nullptr);
}
}

// The display's host1x filter waiter, the work loop not yet run: certain only with the decode's syncpoint at its
// fence, its bridge done, nothing failed and its events batch settled. Every decode carries a batch, which retires
// only after its physical result: one with no waits counts as settled.
TEST_F(SwitchNVDECSubmitTest, TheFilterStatusIsCertainOnlyOnceTheSyncpointIsAtTheFenceWithNothingElseLeft)
{
    const auto lastToken = [] {
        return switch_gpu_backing_token{gQueue.submitted, 1, SWITCH_GPU_ENGINE_NVDEC, gSyncpt};
    };
    ASSERT_EQ(submit(), 0);
    auto plain = lastToken();
    EXPECT_NE(gQueue.jobs[gQueue.head].semantics, nullptr);
    EXPECT_EQ(switch_nvdec_token_filter_status(&plain), 0) << "its syncpoint short of its fence";
    hardware = static_cast<uint32_t>(plain.value);
    EXPECT_EQ(switch_nvdec_token_filter_status(&plain), 1) << "a batch with no waits, before its retirement";
    batchSettled = 0;
    EXPECT_EQ(switch_nvdec_token_filter_status(&plain), 0) << "a batch with waits still out";
    batchSettled = 1;
    auto other = plain;
    other.queue = gSyncpt + 1;
    EXPECT_EQ(switch_nvdec_token_filter_status(&other), 0);
    interrupt(0);
    semantic(plain.value);
    EXPECT_EQ(switch_nvdec_token_filter_status(&plain), 0) << "retired: its slot is no longer the job's";

    gpu();
    ASSERT_EQ(submit(), 0);
    auto bridgedDecode = lastToken();
    hardware = static_cast<uint32_t>(bridgedDecode.value);
    EXPECT_EQ(switch_nvdec_token_filter_status(&bridgedDecode), 0) << "its GPU producer's bridge still out";
    bridged(0, -5);
    EXPECT_EQ(switch_nvdec_token_filter_status(&bridgedDecode), 0);
    interrupt(1);
    semantic(bridgedDecode.value);
}

TEST_F(SwitchNVDECSubmitTest, CompletedHardwareWithFailedProducerRetiresOnlyItsOwnJob)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    const uint64_t failed = gQueue.submitted;
    bridged(0, -5);
    interrupt(0);
    semantic(failed);
    EXPECT_EQ(waitSequence(failed), -5);
    sourceTokens.clear();
    ASSERT_EQ(submit(), 0);
    const uint64_t independent = gQueue.submitted;
    interrupt(1);
    semantic(independent);
    EXPECT_EQ(waitSequence(independent), 0);
    EXPECT_EQ(waitSequence(failed), -5);
    EXPECT_EQ(gQueue.count, 0u);
}

// A decode's bound covers the whole queue ahead of it, from its submission or, behind a GPU producer, from that
// producer's completion (D8); it ends on the interrupt, the bridge and the events' retirement, in any order, on no
// thread of its own.
TEST_F(SwitchNVDECSubmitTest, ADecodeEndsWhenItsFenceBridgeAndSemanticOutcomeAreAllIn)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(fenceBounds.back(), kWaitBoundUs);
    EXPECT_EQ(fences.back()->threshold, fence);
    const uint64_t plain = gQueue.submitted;
    gpu();
    ASSERT_EQ(submit(), 0);
    const uint64_t bridgedDecode = gQueue.submitted;
    EXPECT_EQ(fenceBounds.back(), 0u);
    semantic(plain);
    EXPECT_EQ(gQueue.count, 2u);
    interrupt(0);
    EXPECT_EQ(gQueue.count, 1u);
    interrupt(1);
    semantic(bridgedDecode);
    EXPECT_EQ(gQueue.count, 1u);
    bridged(0, 0);
    EXPECT_EQ(watchedBounds, std::vector<uint32_t>{kWaitBoundUs});
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(gQueue.completed, bridgedDecode);
    EXPECT_EQ(released, 1u);
}

// A decode past its bound fails, never a panic: the jobs behind it fail too, the channel is torn down and restarted
// around the engine's reset, their fences are released, and the next decode runs on the renumbered queue.
TEST_F(SwitchNVDECSubmitTest, AStalledDecodeIsCancelledAndTheNextDecodeRuns)
{
    ASSERT_EQ(submit(), 0);
    const uint64_t stalled = gQueue.submitted;
    ASSERT_EQ(submit(), 0);
    const uint64_t behind = gQueue.submitted;
    semantic(stalled);
    semantic(behind);
    outlived(0);
    EXPECT_EQ(waitSequence(stalled), -5);
    EXPECT_EQ(gQueue.count, 1u);
    EXPECT_THROW(submit(), std::runtime_error) << "new decodes wait for the cancellation";
    IOLockUnlock(gLock);
    runCancellation();
    EXPECT_EQ(engineSteps, (std::vector<std::string>{"teardown", "reset", "release " + std::to_string(uint32_t(behind)),
                                                     "restart", "boot"}));
    EXPECT_EQ(gQueue.count, 1u) << "the cancelled job ends on its released fence";
    EXPECT_TRUE(gCancelling);
    reachedInterrupts();
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(waitSequence(behind), -5);
    EXPECT_FALSE(gCancelling);
    ASSERT_EQ(switch_test_log().size(), 1u);
    EXPECT_NE(switch_test_log()[0].find("TegraNVDEC: job fence " + std::to_string(uint32_t(stalled)) + " not reached"),
              std::string::npos);
    EXPECT_NE(switch_test_log()[0].find("engine reset: running again"), std::string::npos);
    ASSERT_EQ(submit(), 0);
    const uint64_t next = gQueue.submitted;
    EXPECT_EQ(fence, uint32_t(behind) + 3) << "after the proof's two increments";
    semantic(next);
    hardware = fence;
    reachedInterrupts();
    EXPECT_EQ(waitSequence(next), 0);
    EXPECT_EQ(gQueue.count, 0u);
}

TEST_F(SwitchNVDECSubmitTest, EveryFenceOfOneStallJoinsOneCancellation)
{
    ASSERT_EQ(submit(), 0);
    ASSERT_EQ(submit(), 0);
    semantic(gQueue.submitted - 1);
    semantic(gQueue.submitted);
    outlived(0);
    outlived(1);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_TRUE(gCancelling) << "the queue holds new jobs until the engine is back";
    EXPECT_FALSE(switch_nvdec_jobs_park(false)) << "and the idle gate leaves it powered";
    runCancellation();
    EXPECT_FALSE(gCancelling);
    EXPECT_EQ(switch_test_log().size(), 1u);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(fence, static_cast<uint32_t>(nextTestSequence) + 5);
}

TEST_F(SwitchNVDECSubmitTest, AnEngineTheResetDoesNotBringBackRefusesTheNextDecode)
{
    ASSERT_EQ(submit(), 0);
    semantic(gQueue.submitted);
    engineBoots = false;
    outlived(0);
    runCancellation();
    EXPECT_EQ(engineSteps.back(), "wedge");
    ASSERT_EQ(switch_test_log().size(), 1u);
    EXPECT_NE(switch_test_log()[0].find("it did not come back, NVDEC refuses work: falcon did not answer"),
              std::string::npos);
    EXPECT_EQ(submit(), -19);
}

// After an idle power-off (SwitchEngineGate expire), the next decode powers NVDEC on: the power-on's proof moves the
// syncpoint by two, the queue rebases on it, and the decode runs on the next fence.
TEST_F(SwitchNVDECSubmitTest, AnIdleGatedEngineIsPoweredOnByTheNextDecode)
{
    SwitchEngineGate<TestGatePlatform> gate;
    const SwitchEngineGateOps ops = {
        "TegraNVDEC",
        nullptr,
        [](void *) -> int {
            hardware += 2;
            switch_nvdec_jobs_resume(true, hardware);
            return 0;
        },
        [](void *, bool drain) -> int {
            return switch_nvdec_jobs_park(drain) ? SWITCH_ENGINE_GATE_OFF : SWITCH_ENGINE_GATE_BUSY;
        },
    };
    ASSERT_TRUE(gate.start(ops));
    testGate = &gate;
    gate.expire();
    EXPECT_FALSE(gate.powered());
    EXPECT_TRUE(gSuspended);
    const TegraNVDECResolve resolve = [](void *, uint32_t, TegraNVDECBlock *) {
        return false;
    };
    const Check check = [](const TegraNVDECSubmit *, const uint64_t *) -> const char * {
        return nullptr;
    };
    const char *why = nullptr;
    bool waitedForRoom = false;
    EXPECT_EQ(run(&request, resolve, nullptr, check, nullptr, &fence, &waitedForRoom, &why), 0);
    testGate = nullptr;
    EXPECT_TRUE(gate.powered());
    EXPECT_FALSE(gSuspended);
    EXPECT_EQ(fence, static_cast<uint32_t>(nextTestSequence) + 3);
    semantic(gQueue.submitted);
    hardware = fence;
    reachedInterrupts();
    EXPECT_EQ(waitSequence(gQueue.submitted), 0);
    EXPECT_EQ(gQueue.count, 0u);
}

// Idle power gating (TegraNVDEC.cpp, SwitchEngineGate.h): a run holds the powered engine across its publication, an
// idle power-off parks only an empty queue, and the power-on's proof stream moves the syncpoint the queue rebases on.
TEST_F(SwitchNVDECSubmitTest, EveryRunHoldsThePowerOnceAndReleasesIt)
{
    const TegraNVDECResolve resolve = [](void *, uint32_t, TegraNVDECBlock *) {
        return false;
    };
    const Check check = [](const TegraNVDECSubmit *, const uint64_t *) -> const char * {
        return nullptr;
    };
    const char *why = nullptr;
    ASSERT_EQ(run(&request, resolve, nullptr, check, nullptr, &fence, &waitedForRoom, &why), 0);
    EXPECT_EQ(powerUses, 1u);
    EXPECT_EQ(powerDones, 1u);
    powerError = -19;
    const uint32_t before = submits;
    EXPECT_EQ(run(&request, resolve, nullptr, check, nullptr, &fence, &waitedForRoom, &why), -19);
    EXPECT_EQ(submits, before);
    EXPECT_EQ(powerDones, 1u);
}

TEST_F(SwitchNVDECSubmitTest, AnIdlePowerOffParksOnlyAnEmptyQueue)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_FALSE(switch_nvdec_jobs_park(false));
    EXPECT_FALSE(gSuspended);
}

TEST_F(SwitchNVDECSubmitTest, AParkedQueueHoldsTheNextJobUntilAResumeRebasesIt)
{
    ASSERT_TRUE(switch_nvdec_jobs_park(false));
    EXPECT_THROW(submit(), std::runtime_error) << "the job sleeps until resume";
    IOLockUnlock(gLock);
    switch_nvdec_jobs_resume(true, static_cast<uint32_t>(nextTestSequence) + 2);
    EXPECT_FALSE(gSuspended);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(fence, static_cast<uint32_t>(nextTestSequence + 3));
}

namespace {
struct Completed {
    uint32_t fence;
    int result, recordStatus;
    bool queueLocked;
};

std::vector<Completed> completed;

void recordCompletion(void *context, uint32_t fence, int result)
{
    const switch_gpu_backing_token token = {*static_cast<uint64_t *>(context), switch_nvdec_result_state.generation,
                                            SWITCH_GPU_ENGINE_NVDEC, gSyncpt};
    completed.push_back({fence, result, status(&token), switch_test_queue_locked});
}
}  // namespace

// The AVD plug-in's emission hangs off this: a job's completion runs once from its fence-driven retirement, off the
// queue lock, while a Wait on the job would still sleep (its Results record not yet complete).
TEST_F(SwitchNVDECSubmitTest, ACompletionRunsOnceFromRetirementBeforeItsRecordCompletes)
{
    completed.clear();
    uint64_t sequence = nextTestSequence + 1;
    ASSERT_EQ(submit({recordCompletion, &sequence}), 0);
    interrupt(0);
    EXPECT_TRUE(completed.empty()) << "the semantic outcome is not in yet";
    semantic(sequence);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].fence, fence);
    EXPECT_EQ(completed[0].result, 0);
    EXPECT_EQ(completed[0].recordStatus, 0) << "the record completes after the completion";
    EXPECT_FALSE(completed[0].queueLocked);
    EXPECT_EQ(waitSequence(sequence), 0);
    EXPECT_EQ(gQueue.jobs[0].completion.done, nullptr);
}

TEST_F(SwitchNVDECSubmitTest, AFailedProducersJobCompletesWithItsError)
{
    completed.clear();
    gpu();
    uint64_t sequence = nextTestSequence + 1;
    ASSERT_EQ(submit({recordCompletion, &sequence}), 0);
    bridged(0, -5);
    interrupt(0);
    semantic(sequence);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].result, -5);
}

// The AVD plug-in's pictures of a stalled decode and of the decodes queued behind it fail and none is left unanswered:
// each job's completion runs once with the failure, off the queue lock, from its fence (the stalled one's bound, the
// others' released fences) and its semantic outcome in either order.
TEST_F(SwitchNVDECSubmitTest, AStalledDecodeAndTheDecodesBehindItCompleteWithTheFailure)
{
    completed.clear();
    uint64_t stalled = nextTestSequence + 1, behind = nextTestSequence + 2, after = nextTestSequence + 3;
    ASSERT_EQ(submit({recordCompletion, &stalled}), 0);
    ASSERT_EQ(submit({recordCompletion, &behind}), 0);
    ASSERT_EQ(submit({recordCompletion, &after}), 0);
    semantic(behind);
    outlived(0);
    EXPECT_TRUE(completed.empty()) << "the stalled decode's semantic outcome is not in yet";
    semantic(stalled);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].fence, uint32_t(stalled));
    EXPECT_EQ(completed[0].result, -5);
    EXPECT_FALSE(completed[0].queueLocked);
    runCancellation();
    reachedInterrupts();
    ASSERT_EQ(completed.size(), 2u) << "the decode whose semantic outcome came first ends on its released fence";
    EXPECT_EQ(completed[1].fence, uint32_t(behind));
    EXPECT_EQ(completed[1].result, -5);
    EXPECT_TRUE(gCancelling) << "the last cancelled decode has not ended";
    semantic(after);
    ASSERT_EQ(completed.size(), 3u) << "the decode whose semantic outcome came last ends on it";
    EXPECT_EQ(completed[2].fence, uint32_t(after));
    EXPECT_EQ(completed[2].result, -5);
    for (const auto &job : completed)
        EXPECT_FALSE(job.queueLocked);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_FALSE(gCancelling);
    uint64_t next = 0;
    ASSERT_EQ(submit({recordCompletion, &next}), 0);
    next = gQueue.submitted;
    semantic(next);
    hardware = fence;
    reachedInterrupts();
    ASSERT_EQ(completed.size(), 4u);
    EXPECT_EQ(completed[3].result, 0) << "the decode after the cancellation";
}

// A decode whose stream waited for a picture VIC wrote, in a VIC job VIC then cancelled, ran on memory VIC never wrote:
// the job ends with VIC's failure, recorded before VIC's syncpoint was released.
TEST_F(SwitchNVDECSubmitTest, ADecodeAfterACancelledVicJobEndsWithItsFailure)
{
    completed.clear();
    const switch_gpu_backing_token placed = {0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};
    sourceTokens = {placed};
    uint64_t sequence = nextTestSequence + 1;
    ASSERT_EQ(submit({recordCompletion, &sequence}), 0);
    ASSERT_EQ(fences.size(), 2u) << "the decode's fence and VIC's";
    EXPECT_EQ(fences[1]->syncpt, 161u);
    EXPECT_EQ(fences[1]->threshold, 9u);
    producerPhysical[{SWITCH_GPU_ENGINE_VIC, placed.value}] = -5;
    interrupt(1);
    interrupt(0);
    semantic(sequence);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].result, -5);
    const switch_gpu_backing_token decode = {sequence, switch_nvdec_result_state.generation, SWITCH_GPU_ENGINE_NVDEC,
                                             gSyncpt};
    EXPECT_EQ(Results::physicalStatus(&decode), -5);
    EXPECT_EQ(waitSequence(sequence), -5);
}

// A decode behind a stalled VIC job is held up upstream: no bound runs for it, or for the decode queued behind it,
// until VIC's fence comes in (VIC's cancellation releasing it), so NVDEC is never reset for VIC's stall.
TEST_F(SwitchNVDECSubmitTest, AnUpstreamStallNeverResetsNVDEC)
{
    completed.clear();
    const switch_gpu_backing_token placed = {0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};
    sourceTokens = {placed};
    uint64_t first = nextTestSequence + 1, second = nextTestSequence + 2;
    ASSERT_EQ(submit({recordCompletion, &first}), 0);
    sourceTokens.clear();
    ASSERT_EQ(submit({recordCompletion, &second}), 0);
    ASSERT_EQ(fences.size(), 3u);
    EXPECT_EQ(fenceBounds, (std::vector<uint32_t>{0, 0, 0})) << "no bound runs while VIC's job has not come in";
    EXPECT_TRUE(watchedBounds.empty());
    producerPhysical[{SWITCH_GPU_ENGINE_VIC, placed.value}] = -5;
    interrupt(1);
    EXPECT_EQ(watchedBounds, (std::vector<uint32_t>{kWaitBoundUs, kWaitBoundUs}));
    semantic(first);
    semantic(second);
    hardware = static_cast<uint32_t>(gQueue.submitted);
    reachedInterrupts();
    ASSERT_EQ(completed.size(), 2u);
    EXPECT_EQ(completed[0].result, -5);
    EXPECT_EQ(completed[1].result, 0);
    EXPECT_EQ(gCancel->entered, 0u);
    EXPECT_TRUE(engineSteps.empty());
}

// The decode whose VIC input came in and which then stops making progress is NVDEC's own stall: it is cancelled.
TEST_F(SwitchNVDECSubmitTest, AStallAfterTheInputsCameInStillResetsNVDEC)
{
    sourceTokens = {{0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161}};
    ASSERT_EQ(submit(), 0);
    semantic(gQueue.submitted);
    interrupt(1);
    ASSERT_EQ(watchedBounds, std::vector<uint32_t>{kWaitBoundUs});
    outlived(0);
    EXPECT_EQ(waitSequence(gQueue.submitted), -5);
    runCancellation();
    EXPECT_EQ(engineSteps.front(), "teardown");
    EXPECT_NE(std::find(engineSteps.begin(), engineSteps.end(), "reset"), engineSteps.end());
}

// A client whose ring of input buffers is one deeper than the queue never writes one a job still reads: the submit that
// finds the queue full sleeps until the oldest job retires, and says so.
TEST_F(SwitchNVDECSubmitTest, ASubmitIntoAFullQueueSleepsUntilTheOldestRetiresAndSaysSo)
{
    for (uint32_t i = 0; i < SwitchNVDECQueue::kSlots; ++i) {
        ASSERT_EQ(submit(), 0);
        EXPECT_FALSE(waitedForRoom);
    }
    ASSERT_TRUE(gQueue.full());
    switch_test_while_asleep = [] {
        interrupt(0);
        semantic(nextTestSequence + 1);
    };
    ASSERT_EQ(submit(), 0);
    EXPECT_TRUE(waitedForRoom);
    EXPECT_EQ(gQueue.completed, nextTestSequence + 1);
    EXPECT_EQ(fence, nextTestSequence + SwitchNVDECQueue::kSlots + 1);
}
