// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvdec_completions_stubs.h"
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchNVDECCompletions.cpp"

struct switch_gpu_backing {
    int references = 1;
};

extern "C" {
void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->references;
}

void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    EXPECT_GT(backing->references, 0);
    --backing->references;
}
}

namespace {
constexpr uint32_t kHandle = 5, kOffset = 256;
constexpr mach_port_t kPort = 0x1103, kOtherPort = 0x2207;

class SwitchNVDECCompletionsTest : public testing::Test {
protected:
    alignas(64) uint8_t input[4096] = {};
    switch_gpu_backing backing;
    TegraNVDECSubmit submit = {};
    SwitchNVDECCompletions *completions = nullptr;

    void SetUp() override
    {
        switch_test_sent.clear();
        switch_test_released_ports.clear();
        switch_test_rights.clear();
        switch_test_on_release = nullptr;
        submit.entry_count = 2;
        submit.entries[0] = {NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kHandle, 512};
        submit.entries[1] = {NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kHandle, kOffset};
        completions = switch_nvdec_completions_create({switch_test_send, switch_test_refer});
        ASSERT_NE(completions, nullptr);
    }

    void TearDown() override
    {
        switch_nvdec_completions_release(completions);
        EXPECT_EQ(backing.references, 1) << "every armed job gives its status buffer back";
        for (const auto &[port, rights] : switch_test_rights)
            EXPECT_EQ(rights, 0) << "port " << port << ": the connection's teardown leaves no right behind";
    }

    // IOConnectSetNotificationPort as the MIG server runs it: the call brings a send right, which the server releases
    // itself when the registration fails.
    IOReturn listen(mach_port_t port, SwitchNVDECCompletions *to)
    {
        ++switch_test_rights[port];
        const IOReturn result = switch_nvdec_completions_listen(to, port, nullptr);
        if (result)
            --switch_test_rights[port];
        return result;
    }

    IOReturn listen(mach_port_t port)
    {
        return listen(port, completions);
    }

    static bool resolve(void *context, uint32_t handle, TegraNVDECBlock *block)
    {
        auto *test = static_cast<SwitchNVDECCompletionsTest *>(context);
        if (handle != kHandle)
            return false;
        *block = {};
        block->va = test->input;
        block->size = sizeof(test->input);
        block->backing = &test->backing;
        return true;
    }

    SwitchNVDECCompletion arm()
    {
        SwitchNVDECCompletion completion = {};
        EXPECT_EQ(switch_nvdec_completions_arm(completions, &submit, resolve, this, &completion), 0);
        return completion;
    }

    void status(uint32_t error, uint32_t correct)
    {
        nvdec_status_s record = {};
        record.error_status = error;
        record.mbs_correctly_decoded = correct;
        std::memcpy(input + kOffset, &record, sizeof(record));
    }
};

TEST_F(SwitchNVDECCompletionsTest, NothingListeningArmsNothing)
{
    const auto completion = arm();
    EXPECT_EQ(completion.done, nullptr);
    EXPECT_EQ(backing.references, 1);
}

// The AVD plug-in's emission: the job's status, read by the kernel after the job's fence, goes to the registered port
// with the job's fence; the status buffer is held until then.
TEST_F(SwitchNVDECCompletionsTest, AJobsStatusReachesTheRegisteredPortWithItsFence)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    ASSERT_NE(completion.done, nullptr);
    EXPECT_EQ(backing.references, 2);
    status(0x2c, 0);
    completion.done(completion.context, 77, 0);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnSuccess);
    EXPECT_EQ(switch_test_sent[0].arguments, (std::vector<io_user_reference_t>{77, 0x2c, 0}));
    EXPECT_EQ(backing.references, 1);
}

TEST_F(SwitchNVDECCompletionsTest, AFailedJobCompletesWithAnError)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    completion.done(completion.context, 78, -5);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnIOError);
}

// A connection closed with jobs in flight: their completions still read and give back the status buffer, send nothing,
// and the record set goes with the last of them.
TEST_F(SwitchNVDECCompletionsTest, AJobOutlivingItsConnectionSendsNothingAndFreesTheRecords)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvdec_completions_release(completions);
    completions = nullptr;
    EXPECT_EQ(switch_test_released_ports, std::vector<mach_port_t>{kPort});
    completion.done(completion.context, 79, 0);
    EXPECT_TRUE(switch_test_sent.empty());
}

TEST_F(SwitchNVDECCompletionsTest, AFailedSubmitsCompletionIsGivenBackUnsent)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvdec_completions_disarm(&completion);
    EXPECT_TRUE(switch_test_sent.empty());
    EXPECT_EQ(backing.references, 1);
}

TEST_F(SwitchNVDECCompletionsTest, ARecordPerJobInFlightAndThenNoMore)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    std::vector<SwitchNVDECCompletion> armed;
    for (uint32_t i = 0; i < 2 * SwitchNVDECQueue::kSlots; ++i)
        armed.push_back(arm());
    SwitchNVDECCompletion refused = {};
    EXPECT_EQ(switch_nvdec_completions_arm(completions, &submit, resolve, this, &refused), -12);
    EXPECT_EQ(refused.done, nullptr);
    for (const auto &completion : armed)
        completion.done(completion.context, 1, 0);
    EXPECT_EQ(switch_test_sent.size(), armed.size());
}

TEST_F(SwitchNVDECCompletionsTest, ANewPortReplacesTheOldOne)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    ASSERT_EQ(listen(kOtherPort), kIOReturnSuccess);
    EXPECT_EQ(switch_test_released_ports, std::vector<mach_port_t>{kPort});
    EXPECT_EQ(switch_test_rights[kPort], 0);
    EXPECT_EQ(switch_test_rights[kOtherPort], 1);
    const auto completion = arm();
    completion.done(completion.context, 80, 0);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kOtherPort);
}

TEST_F(SwitchNVDECCompletionsTest, AStatusRecordOutsideItsBufferArmsNothing)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    submit.entries[1].offset = sizeof(input) - sizeof(nvdec_status_s) + 4;
    EXPECT_EQ(arm().done, nullptr);
    submit.entries[1].value = kHandle + 1;
    EXPECT_EQ(arm().done, nullptr);
}

// Each registration brings its own send right. The port registered again keeps one, the call's second is released,
// and the connection's teardown releases the one kept.
TEST_F(SwitchNVDECCompletionsTest, ThePortRegisteredAgainKeepsOneRight)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    EXPECT_EQ(switch_test_rights[kPort], 1);
    const auto completion = arm();
    completion.done(completion.context, 81, 0);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    switch_nvdec_completions_close(completions);
    EXPECT_EQ(switch_test_rights[kPort], 0);
    switch_nvdec_completions_close(completions);
    EXPECT_EQ(switch_test_released_ports.size(), 3u) << "a second close releases nothing";
}

// A refused registration takes nothing: the server releases the call's right, and the port kept stays.
TEST_F(SwitchNVDECCompletionsTest, ARefusedRegistrationLeavesItsRightToTheServer)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    EXPECT_EQ(listen(kOtherPort, nullptr), kIOReturnBadArgument);
    EXPECT_EQ(switch_nvdec_completions_listen(completions, MACH_PORT_NULL, nullptr), kIOReturnBadArgument);
    EXPECT_TRUE(switch_test_released_ports.empty());
    EXPECT_EQ(switch_test_rights[kOtherPort], 0);
    EXPECT_EQ(switch_test_rights[kPort], 1);
    const auto completion = arm();
    completion.done(completion.context, 82, -5);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
}

// A cancelled decode's completion (-5) that comes while another port is being registered: once the registration has
// replaced the port under the lock, the completion goes to the new port; the old port's right is released after it,
// and nothing is sent there again.
TEST_F(SwitchNVDECCompletionsTest, ACancelledJobsCompletionDuringASwapGoesToThePortWithARight)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto before = arm(), during = arm();
    before.done(before.context, 83, -5);
    switch_test_on_release = [&](mach_port_t released) {
        EXPECT_EQ(released, kPort);
        during.done(during.context, 84, -5);
    };
    ASSERT_EQ(listen(kOtherPort), kIOReturnSuccess);
    switch_test_on_release = nullptr;
    ASSERT_EQ(switch_test_sent.size(), 2u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnIOError);
    EXPECT_EQ(switch_test_sent[1].port, kOtherPort);
    EXPECT_EQ(switch_test_sent[1].result, kIOReturnIOError);
    EXPECT_EQ(switch_test_rights[kPort], 0);
    EXPECT_EQ(switch_test_rights[kOtherPort], 1);
}

// The same while the port already kept is registered again: its extra right goes, the completion still has one.
TEST_F(SwitchNVDECCompletionsTest, ACancelledJobsCompletionDuringAReRegistrationStillHasARight)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    switch_test_on_release = [&](mach_port_t) {
        completion.done(completion.context, 85, -5);
    };
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    switch_test_on_release = nullptr;
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    EXPECT_EQ(switch_test_rights[kPort], 1);
}

// The connection goes with jobs in flight: its teardown releases the right it kept, and a completion after that sends
// nothing.
TEST_F(SwitchNVDECCompletionsTest, TeardownReleasesTheKeptRightBeforeTheLastCompletion)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    ASSERT_EQ(listen(kOtherPort), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvdec_completions_release(completions);
    completions = nullptr;
    EXPECT_EQ(switch_test_rights[kOtherPort], 0);
    completion.done(completion.context, 86, -5);
    EXPECT_TRUE(switch_test_sent.empty());
}
}  // namespace
