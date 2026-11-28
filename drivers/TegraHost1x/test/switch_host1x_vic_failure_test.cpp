// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt into a real queue's cancellation: VIC's queue (SwitchVICJobs.cpp, TegraVICCancel.cpp)
// submits on the real host1x (switch_host1x_vic_failure_host1x.cpp), an illegal opcode stops VIC's channel, and the job
// fails through VIC's own cancellation, whose channel restart unmasks the channel's error again. Only VIC's engine,
// power and GPU-side surroundings are stood in for.
#include "switch_host1x_fence_stubs.h"
#include <gtest/gtest.h>
#include <string>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "switch_host1x_vic_failure.h"
#include "../SwitchHost1xErrors.h"
#include "../SwitchHost1xIntr.h"
#include "../../TegraVIC/SwitchVICJobs.cpp"
#include "../../TegraVIC/TegraVICCancel.cpp"

struct switch_gpu_bridge {};

namespace {
IOLock *gPowerLock;
std::vector<std::string> gEngineSteps;
uint32_t gVicSyncpt;

uint32_t &syncpt(uint32_t id)
{
    return switch_test_sync(0xf80 + 4 * id);
}
}  // namespace

extern "C" {
int switch_gpu_backing_register_engine(uint32_t, const switch_gpu_backing_engine *)
{
    return 0;
}

int switch_gpu_backing_register_filter_status(uint32_t, switch_gpu_backing_filter_status)
{
    return 0;
}

int switch_gpu_backing_register_failure(uint32_t, switch_gpu_backing_failure)
{
    return 0;
}

int switch_gpu_backing_tokens_failure(const switch_gpu_backing_token *, uint32_t)
{
    return 0;
}

int switch_gpu_bridge_why(const switch_gpu_bridge *)
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
    return 0;
}

int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC ? Results::physicalStatus(token) : 1;
}

int switch_gpu_backing_token_retain(const switch_gpu_backing_token *)
{
    return 0;
}

void switch_gpu_backing_token_release(const switch_gpu_backing_token *)
{}

int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count, const switch_gpu_backing_guard *,
                                     uint32_t, const switch_gpu_backing_token *completion,
                                     switch_gpu_backing_transaction *transaction)
{
    *transaction = {};
    transaction->uses = uses;
    transaction->count = count;
    transaction->locked = 1;
    transaction->completion = *completion;
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

int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *, switch_gpu_backing_token *, uint32_t,
                                    uint32_t *needed)
{
    *needed = 0;
    return 0;
}

int switch_gpu_backing_promise_dependencies(switch_gpu_backing_transaction *transaction, switch_gpu_backing_token *,
                                            uint32_t, uint32_t *, uint32_t, uint32_t)
{
    transaction->promised = 1;
    return 0;
}

void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
    transaction->locked = 0;
}

void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
    transaction->locked = 0;
}

int switch_gpu_bridge_submit(const switch_gpu_backing_token *, uint32_t, switch_gpu_bridge **)
{
    throw std::runtime_error("the job has no GPU producer to bridge");
}

void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *, uint32_t *, uint32_t *)
{}

int switch_gpu_bridge_covers(const switch_gpu_bridge *, const switch_gpu_backing_token *)
{
    return 0;
}

int switch_gpu_bridge_failed(const switch_gpu_bridge *)
{
    return 0;
}

int switch_gpu_bridge_source_failure(const switch_gpu_bridge *)
{
    return 0;
}

int switch_gpu_bridge_status(const switch_gpu_bridge *)
{
    return 1;
}

int switch_gpu_bridge_notify(const switch_gpu_bridge *, void (*)(void *, int), void *)
{
    return 0;
}

void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    *bridge = nullptr;
}

int switch_gpu_bridge_wait_room()
{
    return 0;
}

void tegra_vic_describe(char *, size_t)
{}

int switch_vic_power_use()
{
    return 0;
}

void switch_vic_power_done()
{}

void switch_vic_power_idle()
{}

void switch_vic_power_lost()
{
    gEngineSteps.emplace_back("lost");
}

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
    return 1;
}

int switch_gpu_event_waits_settled_status(const switch_gpu_event_waits *)
{
    return 1;
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
    *out = nullptr;
    return 0;
}

int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *, switch_gpu_event_waits *)
{
    return 0;
}
}

IOLock *switch_vic_power_lock()
{
    return gPowerLock;
}

bool switch_vic_engine_powered()
{
    return true;
}

void switch_vic_engine_describe(char *text, size_t size)
{
    snprintf(text, size, "falcon CPUCTL 0");
}

bool switch_vic_engine_falcon_failed(char *, size_t)
{
    return false;
}

bool switch_vic_engine_reset(bool *drained)
{
    *drained = true;
    gEngineSteps.emplace_back("reset");
    return true;
}

// V2's proof after the reset: the rebooted falcon increments the job syncpoint twice.
bool switch_vic_engine_boot()
{
    gEngineSteps.emplace_back("boot");
    syncpt(gVicSyncpt) += 2;
    return true;
}

const char *switch_vic_engine_wedge()
{
    gEngineSteps.emplace_back("wedge");
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

namespace {
using namespace switch_host1x_errors;
constexpr uint32_t kVicChannel = TEGRA_HOST1X_CHANNEL_VIC, kBase = 0x100;

struct Host1xVicFailure : testing::Test {
    uint8_t configurations[kSwitchVICConfigBytes] = {};
    TegraVICJob jobs[1] = {};
    switch_gpu_backing_use uses[1] = {{reinterpret_cast<switch_gpu_backing *>(1), SWITCH_GPU_BACKING_WRITE, 0}};

    void SetUp() override
    {
        switch_test_sleep = nullptr;
        switch_test_now = 1000;
        ASSERT_TRUE(switch_test_host1x_start());
        ASSERT_EQ(tegra_host1x_syncpt_alloc(&gVicSyncpt), TEGRA_HOST1X_OK);
        syncpt(gVicSyncpt) = kBase;
        gQueue = {};
        gQueue.submitted = gQueue.completed = gCompleted = kBase;
        gFailed = gSuspended = gCancelling = gRecovered = gRecoveredReady = false;
        gPowerLock = IOLockAlloc();
        ASSERT_TRUE(switch_vic_cancel_start(gVicSyncpt));
        for (auto &stage : Stages::stages)
            stage = {};
        gLock = IOLockAlloc();
        gSyncpt = gVicSyncpt;
        ASSERT_TRUE(Results::start(gSyncpt));
        gConfigs = {0x90000000, sizeof(configurations), configurations};
        jobs[0].count = 1;
        jobs[0].method[0] = TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET;
    }

    int submit(void (*completion)(void *, int), void *context)
    {
        const SwitchVICOwnership ownership = {uses, 1, nullptr, 0, nullptr, 0};
        return switch_vic_submit_checked(jobs, 1, nullptr, ownership, completion, context, nullptr);
    }

    // The channel's command processor latches an illegal opcode. True when CPU0's line carries it (all four masks
    // open, TRM 14.9.2-14.9.8) and host1x's work loop ran its filter and action.
    static bool illegalOpcode(uint32_t channel)
    {
        switch_test_sync(kHintStatusExt) |= 1u << channel;
        switch_test_sync(kCmdprocStat) |= 1u << channel;
        const bool line = (switch_test_sync(kHintStatusExt) & switch_test_sync(kHintMaskExt)) &&
                          (switch_test_sync(kHintMask) & kExtEnable) && (switch_test_sync(kIntC0Mask) & kHostInt) &&
                          (switch_test_sync(kIntMask) & kCpu0All);
        auto *general = IOFilterInterruptEventSource::made().back();
        if (!line || !general->filter(nullptr, general))
            return false;
        general->action(nullptr, general, 1);
        return true;
    }

    // The threshold interrupt for the job syncpoint, on host1x's work loop.
    static void reached(uint32_t value)
    {
        syncpt(gVicSyncpt) = value;
        switch_test_sync(0xe80 + 4 * (gVicSyncpt / 32)) |= 1u << (gVicSyncpt % 32);
        switch_host1x_intr_handle();
    }
};

bool anyLine(const std::string &text)
{
    for (const auto &line : switch_test_iolog)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

void heard(void *context, int status)
{
    *static_cast<int *>(context) = status;
}
}  // namespace

TEST_F(Host1xVicFailure, AnIllegalOpcodeOnVicsChannelFailsItsJobThroughVicsCancellationAndTheRestartRearmsIt)
{
    int stopped = 1, next = 1;
    ASSERT_EQ(submit(heard, &stopped), 0);
    const uint32_t threshold = kBase + 1;
    switch_test_bounds = nullptr;

    ASSERT_TRUE(illegalOpcode(kVicChannel));
    EXPECT_TRUE(anyLine("TegraHost1x: channel 1 stopped on an illegal opcode"));
    EXPECT_TRUE(anyLine("syncpoint " + std::to_string(gVicSyncpt) + ": 1 failed"));
    EXPECT_EQ(switch_test_sync(kHintMaskExt) & (1u << kVicChannel), 0u) << "masked while the channel is stopped";
    switch_host1x_intr_handle();
    EXPECT_EQ(stopped, Stages::kCancelled) << "failed at once, as its bound would have failed it";
    ASSERT_EQ(gCancel->entered, 1u) << "VIC's own cancellation, as for a stalled job";
    EXPECT_EQ(switch_test_bounds, nullptr) << "no bound ran out";

    gCancel->entered = 0;
    gCancel->fire(gCancel->param, nullptr);
    EXPECT_EQ(gEngineSteps, (std::vector<std::string>{"reset", "boot"}));
    EXPECT_TRUE(anyLine("engine reset: running again"));
    EXPECT_GE(static_cast<int32_t>(syncpt(gVicSyncpt) - threshold), 0) << "the cancellation released the job's fence";
    EXPECT_EQ(switch_test_sync(kHintMaskExt), kIllegalAll) << "VIC's restart of its channel unmasked it again";
    EXPECT_EQ(switch_test_sync(kHintStatusExt), 0u);
    EXPECT_FALSE(gCancelling);
    EXPECT_EQ(gQueue.count, 0u);

    ASSERT_EQ(submit(heard, &next), 0);
    reached(static_cast<uint32_t>(gQueue.submitted));
    EXPECT_EQ(next, 0) << "the next job on the restarted channel runs: its syncpoint is no longer marked stopped";
    EXPECT_EQ(gQueue.count, 0u);
}
