// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvenc_completions_stubs.h"
#define SWITCH_DCACHE_OP(op, address) switch_test_cache_ops.push_back({(op), static_cast<uintptr_t>(address)})
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchNVENCCompletions.cpp"

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
constexpr uint32_t kStatusHandle = 5, kStatusOffset = 256;
constexpr uint32_t kBitsHandle = 6, kBitsOffset = 128, kBitsLimit = 4096;
constexpr mach_port_t kPort = 0x1103, kOtherPort = 0x2207;

class SwitchNVENCCompletionsTest : public testing::Test {
protected:
    alignas(64) uint8_t statusBuf[4096] = {};
    alignas(64) uint8_t bitsBuf[8192] = {};
    switch_gpu_backing statusBacking;
    switch_gpu_backing bitsBacking;
    TegraNVENCSubmit submit = {};
    SwitchNVENCCompletions *completions = nullptr;

    void SetUp() override
    {
        switch_test_sent.clear();
        switch_test_released_ports.clear();
        switch_test_rights.clear();
        switch_test_on_release = nullptr;
        switch_test_cache_ops.clear();
        setH264Submit(0, kBitsLimit);
        completions = switch_nvenc_completions_create({switch_test_send, switch_test_refer});
        ASSERT_NE(completions, nullptr);
    }

    void TearDown() override
    {
        switch_nvenc_completions_release(completions);
        EXPECT_EQ(statusBacking.references, 1);
        EXPECT_EQ(bitsBacking.references, 1);
        for (const auto &[port, rights] : switch_test_rights)
            EXPECT_EQ(rights, 0) << "port " << port << " leaked a send right";
    }

    void setH264Submit(uint32_t startPos, uint32_t limit)
    {
        submit = {};
        submit.codec = TEGRA_NVENC_CODEC_H264;
        submit.entry_count = 2;
        submit.entries[0] = {NVC5B7_SET_OUT_ENC_STATUS, TEGRA_NVENC_ENTRY_RELOC, kStatusHandle, kStatusOffset};
        submit.entries[1] = {NVC5B7_SET_OUT_BITSTREAM, TEGRA_NVENC_ENTRY_RELOC, kBitsHandle, kBitsOffset};
        nvenc_h264_drv_pic_setup_s setup = {};
        setup.pic_control.bitstream_start_pos = startPos;
        setup.pic_control.bitstream_buf_size = limit;
        submit.setup_bytes = sizeof(setup);
        std::memcpy(submit.setup, &setup, sizeof(setup));
    }

    void setH265Submit(uint32_t startPos, uint32_t limit)
    {
        submit = {};
        submit.codec = TEGRA_NVENC_CODEC_H265;
        submit.entry_count = 2;
        submit.entries[0] = {NVC5B7_SET_OUT_ENC_STATUS, TEGRA_NVENC_ENTRY_RELOC, kStatusHandle, kStatusOffset};
        submit.entries[1] = {NVC5B7_SET_OUT_BITSTREAM, TEGRA_NVENC_ENTRY_RELOC, kBitsHandle, kBitsOffset};
        nvenc_h265_drv_pic_setup_s setup = {};
        setup.pic_control.bitstream_start_pos = startPos;
        setup.pic_control.bitstream_buf_size = limit;
        submit.setup_bytes = sizeof(setup);
        std::memcpy(submit.setup, &setup, sizeof(setup));
    }

    IOReturn listen(mach_port_t port, SwitchNVENCCompletions *to)
    {
        ++switch_test_rights[port];
        const IOReturn result = switch_nvenc_completions_listen(to, port, nullptr);
        if (result)
            --switch_test_rights[port];
        return result;
    }

    IOReturn listen(mach_port_t port)
    {
        return listen(port, completions);
    }

    static bool resolve(void *context, uint32_t handle, TegraNVENCBlock *block)
    {
        auto *test = static_cast<SwitchNVENCCompletionsTest *>(context);
        *block = {};
        if (handle == kStatusHandle) {
            block->va = test->statusBuf;
            block->size = sizeof(test->statusBuf);
            block->backing = &test->statusBacking;
            return true;
        }
        if (handle == kBitsHandle) {
            block->va = test->bitsBuf;
            block->size = sizeof(test->bitsBuf);
            block->backing = &test->bitsBacking;
            return true;
        }
        return false;
    }

    SwitchNVENCCompletion arm()
    {
        SwitchNVENCCompletion completion = {};
        EXPECT_EQ(switch_nvenc_completions_arm(completions, &submit, resolve, this, &completion), 0);
        return completion;
    }

    void writeStatus(uint32_t errorStatus, uint32_t ucodeError, uint32_t totalBits)
    {
        nvenc_pic_stat_s record = {};
        record.error_status = errorStatus;
        record.ucode_error_status = ucodeError;
        record.total_bit_count = totalBits;
        std::memcpy(statusBuf + kStatusOffset, &record, sizeof(record));
    }

    bool invalidatedLine(const uint8_t *base, size_t offset) const
    {
        const uintptr_t line = reinterpret_cast<uintptr_t>(base + offset) & ~uintptr_t(SWITCH_DCACHE_LINE - 1);
        for (const auto &op : switch_test_cache_ops)
            if (op.address == line)
                return true;
        return false;
    }
};

TEST_F(SwitchNVENCCompletionsTest, NothingListeningArmsNothing)
{
    const auto completion = arm();
    EXPECT_EQ(completion.done, nullptr);
    EXPECT_EQ(statusBacking.references, 1);
    EXPECT_EQ(bitsBacking.references, 1);
}

TEST_F(SwitchNVENCCompletionsTest, H264CompletionInvalidatesStatusAndProducedBitstreamSliceOnly)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    ASSERT_NE(completion.done, nullptr);
    EXPECT_EQ(statusBacking.references, 2);
    EXPECT_EQ(bitsBacking.references, 2);

    writeStatus(2, 0, 200 * 8);
    completion.done(completion.context, 77, 0);

    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnSuccess);
    EXPECT_EQ(switch_test_sent[0].arguments, (std::vector<io_user_reference_t>{77, 2, 0, 1600}));
    EXPECT_EQ(statusBacking.references, 1);
    EXPECT_EQ(bitsBacking.references, 1);

    EXPECT_TRUE(invalidatedLine(statusBuf, kStatusOffset));
    EXPECT_TRUE(invalidatedLine(statusBuf, kStatusOffset + sizeof(nvenc_pic_stat_s) - 1));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset + 199));
    EXPECT_FALSE(invalidatedLine(bitsBuf, kBitsOffset + 256));
}

TEST_F(SwitchNVENCCompletionsTest, H265CompletionRespectsStartPosAndClampsToLimit)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    setH265Submit(128, 384);
    const auto completion = arm();
    ASSERT_NE(completion.done, nullptr);

    writeStatus(2, 0, 1024 * 8);  // larger than remaining 256 bytes: must clamp
    completion.done(completion.context, 88, 0);

    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].arguments, (std::vector<io_user_reference_t>{88, 2, 0, 8192}));
    EXPECT_FALSE(invalidatedLine(bitsBuf, kBitsOffset));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset + 128));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset + 383));
    EXPECT_FALSE(invalidatedLine(bitsBuf, kBitsOffset + 384));
}

TEST_F(SwitchNVENCCompletionsTest, UcodeErrorSkipsBitstreamInvalidation)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    writeStatus(2, 0x1234, 512 * 8);
    completion.done(completion.context, 90, 0);

    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].arguments, (std::vector<io_user_reference_t>{90, 2, 0x1234, 4096}));
    EXPECT_TRUE(invalidatedLine(statusBuf, kStatusOffset));
    EXPECT_FALSE(invalidatedLine(bitsBuf, kBitsOffset));
}

TEST_F(SwitchNVENCCompletionsTest, FailedJobSkipsInvalidationAndSendsIOError)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    writeStatus(2, 0, 512 * 8);
    completion.done(completion.context, 91, -5);

    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnIOError);
    EXPECT_TRUE(switch_test_cache_ops.empty());
}

TEST_F(SwitchNVENCCompletionsTest, SingleWaitInvalidationSkipsRedundantStatusInvalidateWhenCovered)
{
    SwitchNVENCOutputSpec spec = {};
    ASSERT_TRUE(switch_nvenc_output_spec(&submit, &spec));
    EXPECT_TRUE(switch_nvenc_status_range_covered(&spec, kStatusHandle, kStatusOffset, sizeof(nvenc_pic_stat_s)));
    EXPECT_FALSE(switch_nvenc_status_range_covered(&spec, kStatusHandle, kStatusOffset, 64));
    EXPECT_FALSE(switch_nvenc_status_range_covered(&spec, kBitsHandle, kStatusOffset, sizeof(nvenc_pic_stat_s)));

    writeStatus(2, 0, 128 * 8);
    ASSERT_TRUE(switch_nvenc_invalidate_spec(&spec, resolve, this, true, nullptr));
    EXPECT_FALSE(invalidatedLine(statusBuf, kStatusOffset));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset + 127));
    EXPECT_FALSE(invalidatedLine(bitsBuf, kBitsOffset + 128));

    switch_test_cache_ops.clear();
    ASSERT_TRUE(switch_nvenc_invalidate_spec(&spec, resolve, this, false, nullptr));
    EXPECT_TRUE(invalidatedLine(statusBuf, kStatusOffset));
    EXPECT_TRUE(invalidatedLine(bitsBuf, kBitsOffset));
}

TEST_F(SwitchNVENCCompletionsTest, DisarmAndPoolLimitAndPortRights)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    EXPECT_EQ(switch_test_rights[kPort], 1);

    const auto disarmed = arm();
    switch_nvenc_completions_disarm(&disarmed);
    EXPECT_TRUE(switch_test_sent.empty());
    EXPECT_EQ(statusBacking.references, 1);
    EXPECT_EQ(bitsBacking.references, 1);

    std::vector<SwitchNVENCCompletion> armed;
    for (uint32_t i = 0; i < kSwitchNVENCCompletionRecords; ++i)
        armed.push_back(arm());
    SwitchNVENCCompletion refused = {};
    EXPECT_EQ(switch_nvenc_completions_arm(completions, &submit, resolve, this, &refused), -12);
    EXPECT_EQ(refused.done, nullptr);

    ASSERT_EQ(listen(kOtherPort), kIOReturnSuccess);
    EXPECT_EQ(switch_test_rights[kPort], 0);
    EXPECT_EQ(switch_test_rights[kOtherPort], 1);
    for (const auto &c : armed)
        c.done(c.context, 1, 0);
    EXPECT_EQ(switch_test_sent.size(), armed.size());
    EXPECT_EQ(switch_test_sent[0].port, kOtherPort);
}

TEST_F(SwitchNVENCCompletionsTest, JobOutlivingConnectionSendsNothingAndFreesRecords)
{
    ASSERT_EQ(listen(kPort), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvenc_completions_release(completions);
    completions = nullptr;
    EXPECT_EQ(switch_test_rights[kPort], 0);
    completion.done(completion.context, 99, 0);
    EXPECT_TRUE(switch_test_sent.empty());
}
}  // namespace
