// SPDX-License-Identifier: MIT
// The GPU's L2 is not coherent with the CPU: the kernel drops its lines of memory the CPU wrote (L2_SYSMEM_INVALIDATE,
// gpu_exec.c) only for a job it knows the write came before. raw::Device over the kernel model (raw_device_harness.h),
// whose SubmitJob says which jobs the kernel takes as host-written.
#include "raw_device_harness.h"

namespace {

constexpr uint32_t kBgra8 = 80, kSide = 16;

class HostWritten : public harness::DeviceTest {
protected:
    raw::Texture target, other;

    void SetUp() override
    {
        harness::DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, target, true));
        ASSERT_TRUE(dev.createTexture(kSide, kSide, kBgra8, other, true));
        ASSERT_TRUE(target.wc);
    }

    // One job copying `from` into `to`: whether the kernel took it as host-written
    bool copy(const raw::Texture &from, const raw::Texture &to)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e) {
            ADD_FAILURE() << "no blit encoder";
            return false;
        }
        e->copyTexels(from, 0, 0, 0, to, 0, 0, 0, kSide, kSide);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return !kernel_model::model.jobs.empty() && kernel_model::model.jobs.back().hostWritten;
    }

    uint64_t bytes() const
    {
        return uint64_t(target.pitch) * kSide;
    }
};

// The bench's fill before it took a lease: the GPU wrote the target, the CPU rewrote it with a bare memcpy, and the next
// job's ROP read the GPU's older lines from the L2
TEST_F(HostWritten, ABareCpuWriteTellsTheKernelNothing)
{
    copy(other, target);
    memset(target.cpu, 0x5a, bytes());
    EXPECT_FALSE(copy(target, other));
}

// Shared textures' replaceRegion (TextureCpuAccess) and now the bench's fill
TEST_F(HostWritten, AWriteUnderALeaseMakesTheNextJobThatReadsItDropTheL2First)
{
    copy(other, target);
    {
        raw::HostAccess lease;
        ASSERT_TRUE(dev.hostAccess(target, target.cpu, bytes(), true, lease));
        memset(target.cpu, 0x5a, bytes());
    }
    EXPECT_TRUE(copy(target, other));
    EXPECT_FALSE(copy(target, other)) << "a later job: the L2 holds no line older than the CPU's write";
}

TEST_F(HostWritten, AReadLeaseIsNoWrite)
{
    copy(other, target);
    {
        raw::HostAccess lease;
        ASSERT_TRUE(dev.hostAccess(target, target.cpu, bytes(), false, lease));
    }
    EXPECT_FALSE(copy(target, other));
}

}  // namespace
