// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_host_access.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

namespace {
struct Call {
    uint64_t handle, offset, bytes;
    uint32_t access;
    bool acquire;
};

std::vector<Call> calls;
bool acquireFails, releaseFails;
uint32_t reports;
std::array<uint8_t, 128> cpu, memory;
}  // namespace

namespace raw {
Kernel::~Kernel() = default;

void reportCpuReleaseFailure()
{
    ++reports;
}

bool Kernel::cpuAccess(const Bo &bo, uint64_t offset, uint64_t bytes, uint32_t access, bool acquire)
{
    calls.push_back({bo.handle, offset, bytes, access, acquire});
    if (acquire ? acquireFails : releaseFails)
        return false;
    const uint64_t begin = offset & ~uint64_t(63), end = (offset + bytes + 63) & ~uint64_t(63);
    if (acquire)
        std::memcpy(cpu.data() + begin, memory.data() + begin, end - begin);
    else if (access & TegraGM20BCpuWrite)
        std::memcpy(memory.data() + begin, cpu.data() + begin, end - begin);
    return true;
}
}  // namespace raw

class RawHostAccess : public testing::Test {
protected:
    raw::Kernel kernel;
    raw::Bo bo;

    void SetUp() override
    {
        calls.clear();
        acquireFails = releaseFails = false;
        reports = 0;
        cpu.fill(0x19);
        memory.fill(0xa7);
        bo.handle = 7;
        bo.cpu = cpu.data();
        bo.va = bo.vaBase = 0x10000;
        bo.bytes = cpu.size();
    }
};

TEST_F(RawHostAccess, PartialWritePreservesGpuBytesInTheSameCacheLine)
{
    {
        raw::HostAccess access;
        ASSERT_TRUE(access.acquire(kernel, bo, cpu.data() + 7, 2, true));
        cpu[7] = 0x31;
        cpu[8] = 0x42;
    }
    for (size_t i = 0; i < memory.size(); ++i)
        EXPECT_EQ(memory[i], i == 7 ? 0x31 : i == 8 ? 0x42 : 0xa7);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_TRUE(calls[0].acquire);
    EXPECT_FALSE(calls[1].acquire);
    EXPECT_EQ(calls[0].access, uint32_t(TegraGM20BCpuWrite));
    EXPECT_EQ(calls[1].offset, 7u);
    EXPECT_EQ(calls[1].bytes, 2u);
}

TEST_F(RawHostAccess, InteriorImportUsesPhysicalBackingOffset)
{
    bo.cpu += 13;
    bo.va += 13;
    raw::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, bo, cpu.data() + 19, 3, false));
    EXPECT_EQ(cpu[19], memory[19]);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].offset, 19u);
    EXPECT_EQ(calls[0].access, uint32_t(TegraGM20BCpuRead));
    ASSERT_TRUE(access.finish());
    ASSERT_TRUE(access.finish());
    EXPECT_EQ(calls.size(), 2u);
}

TEST_F(RawHostAccess, InvalidAndFailedAcquireDoNotReleaseOwnership)
{
    {
        raw::HostAccess access;
        EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 0, true));
        EXPECT_FALSE(access.acquire(kernel, bo, cpu.data() + 127, 2, true));
        EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), UINT64_MAX, true));
        EXPECT_TRUE(calls.empty());
        acquireFails = true;
        EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 64, true));
    }
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].acquire);
}

TEST_F(RawHostAccess, CannotReplaceAnActiveLeaseAndFailedReleaseRemainsRetryable)
{
    raw::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, bo, cpu.data(), 64, false));
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data() + 64, 64, false));
    releaseFails = true;
    EXPECT_FALSE(access.finish());
    releaseFails = false;
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(calls.size(), 3u);
}

TEST_F(RawHostAccess, DestructorReportsUnreleasedOwnership)
{
    {
        raw::HostAccess access;
        ASSERT_TRUE(access.acquire(kernel, bo, cpu.data(), 64, false));
        releaseFails = true;
    }
    EXPECT_EQ(reports, 1u);
}

TEST_F(RawHostAccess, ViewAndHeapOffsetsKeepPartialCachelineNeighbors)
{
    const void *owner = cpu.data() + 16, *view = cpu.data() + 51;
    ASSERT_TRUE(raw::hostRangeContains(owner, 96, view, 18));
    ASSERT_FALSE(raw::hostRangeContains(owner, 48, view, 18));
    raw::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, bo, view, 18, true));
    for (unsigned at : {51u, 52u, 67u, 68u})
        cpu[at] = 0x31;
    ASSERT_TRUE(access.finish());
    for (unsigned at = 0; at < memory.size(); ++at)
        EXPECT_EQ(memory[at], at == 51 || at == 52 || at == 67 || at == 68 ? 0x31 : 0xa7);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].offset, 51u);
    EXPECT_EQ(calls[0].bytes, 18u);
}

TEST_F(RawHostAccess, InvalidImportedBaseCannotWrapBeforeAcquire)
{
    raw::HostAccess access;
    bo.va = bo.vaBase - 1;
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 64, false));
    bo.vaBase = 0;
    bo.va = UINT64_MAX;
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 64, false));
    EXPECT_FALSE(raw::hostRangeContains(cpu.data() + 64, 64, cpu.data(), 1));
    EXPECT_FALSE(raw::hostRangeContains(cpu.data(), 128, cpu.data() + 127, UINT64_MAX));
    EXPECT_TRUE(calls.empty());
}
