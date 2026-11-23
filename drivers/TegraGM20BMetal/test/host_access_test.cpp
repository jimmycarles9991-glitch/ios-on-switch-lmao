// SPDX-License-Identifier: MIT
#include "../host_access.h"
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
uint64_t refusedAcquireHandle;
uint32_t reports;
std::array<uint8_t, 128> cpu, memory;
}  // namespace

namespace gm20b {
Kernel::~Kernel() = default;

void reportCpuReleaseFailure()
{
    ++reports;
}

bool Kernel::cpuAccess(const Bo &bo, uint64_t offset, uint64_t bytes, uint32_t access, bool acquire)
{
    calls.push_back({bo.handle, offset, bytes, access, acquire});
    if (acquire ? acquireFails || bo.handle == refusedAcquireHandle : releaseFails)
        return false;
    const uint64_t begin = offset & ~uint64_t(63), end = (offset + bytes + 63) & ~uint64_t(63);
    if (acquire)
        std::memcpy(cpu.data() + begin, memory.data() + begin, end - begin);
    else if (access & TegraGM20BCpuWrite)
        std::memcpy(memory.data() + begin, cpu.data() + begin, end - begin);
    return true;
}
}  // namespace gm20b

class GM20BHostAccess : public testing::Test {
protected:
    gm20b::Kernel kernel;
    gm20b::Bo bo;

    void SetUp() override
    {
        calls.clear();
        acquireFails = releaseFails = false;
        refusedAcquireHandle = 0;
        reports = 0;
        cpu.fill(0x19);
        memory.fill(0xa7);
        bo.handle = 7;
        bo.cpu = cpu.data();
        bo.va = bo.vaBase = 0x10000;
        bo.bytes = cpu.size();
    }
};

TEST_F(GM20BHostAccess, PartialWritePreservesGpuBytesInTheSameCacheLine)
{
    {
        gm20b::HostAccess access;
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

TEST_F(GM20BHostAccess, InteriorImportUsesPhysicalBackingOffset)
{
    bo.cpu += 13;
    bo.va += 13;
    gm20b::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, bo, cpu.data() + 19, 3, false));
    EXPECT_EQ(cpu[19], memory[19]);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].offset, 19u);
    EXPECT_EQ(calls[0].access, uint32_t(TegraGM20BCpuRead));
    ASSERT_TRUE(access.finish());
    ASSERT_TRUE(access.finish());
    EXPECT_EQ(calls.size(), 2u);
}

TEST_F(GM20BHostAccess, InvalidAndFailedAcquireDoNotReleaseOwnership)
{
    {
        gm20b::HostAccess access;
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

TEST_F(GM20BHostAccess, CannotReplaceAnActiveLeaseAndFailedReleaseRemainsRetryable)
{
    gm20b::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, bo, cpu.data(), 64, false));
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data() + 64, 64, false));
    releaseFails = true;
    EXPECT_FALSE(access.finish());
    releaseFails = false;
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(calls.size(), 3u);
}

TEST_F(GM20BHostAccess, DestructorReportsUnreleasedOwnership)
{
    {
        gm20b::HostAccess access;
        ASSERT_TRUE(access.acquire(kernel, bo, cpu.data(), 64, false));
        releaseFails = true;
    }
    EXPECT_EQ(reports, 1u);
}

TEST_F(GM20BHostAccess, ViewAndHeapOffsetsKeepPartialCachelineNeighbors)
{
    const void *owner = cpu.data() + 16, *view = cpu.data() + 51;
    ASSERT_TRUE(gm20b::hostRangeContains(owner, 96, view, 18));
    ASSERT_FALSE(gm20b::hostRangeContains(owner, 48, view, 18));
    gm20b::HostAccess access;
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

TEST_F(GM20BHostAccess, InvalidImportedBaseCannotWrapBeforeAcquire)
{
    gm20b::HostAccess access;
    bo.va = bo.vaBase - 1;
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 64, false));
    bo.vaBase = 0;
    bo.va = UINT64_MAX;
    EXPECT_FALSE(access.acquire(kernel, bo, cpu.data(), 64, false));
    EXPECT_FALSE(gm20b::hostRangeContains(cpu.data() + 64, 64, cpu.data(), 1));
    EXPECT_FALSE(gm20b::hostRangeContains(cpu.data(), 128, cpu.data() + 127, UINT64_MAX));
    EXPECT_TRUE(calls.empty());
}

TEST_F(GM20BHostAccess, MultipartAcquireFailureUnwindsBeforeReleasingTheArena)
{
    gm20b::Bo second = bo, third = bo;
    second.handle = 8;
    third.handle = 9;
    refusedAcquireHandle = second.handle;
    std::vector<size_t> unpins;
    gm20b::HostAccess access;
    EXPECT_FALSE(access.acquire(kernel, {{bo, 1, 1}, {second, 2, 1}, {third, 3, 1}}, true, &unpins, 9,
                                [](void *owner, uint32_t arena) {
                                    EXPECT_EQ(arena, 9u);
                                    static_cast<std::vector<size_t> *>(owner)->push_back(calls.size());
                                }));
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].handle, bo.handle);
    EXPECT_TRUE(calls[0].acquire);
    EXPECT_EQ(calls[1].handle, second.handle);
    EXPECT_TRUE(calls[1].acquire);
    EXPECT_EQ(calls[2].handle, bo.handle);
    EXPECT_FALSE(calls[2].acquire);
    EXPECT_EQ(unpins, (std::vector<size_t>{3}));
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(unpins.size(), 1u);
}

TEST_F(GM20BHostAccess, MultipartReleaseReleasesTheArenaAfterEveryRange)
{
    gm20b::Bo second = bo, third = bo;
    second.handle = 8;
    third.handle = 9;
    std::vector<size_t> unpins;
    gm20b::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, {{bo, 1, 1}, {second, 2, 1}, {third, 3, 1}}, false, &unpins, 9,
                               [](void *owner, uint32_t arena) {
                                   EXPECT_EQ(arena, 9u);
                                   static_cast<std::vector<size_t> *>(owner)->push_back(calls.size());
                               }));
    ASSERT_TRUE(access.finish());
    ASSERT_EQ(calls.size(), 6u);
    EXPECT_EQ(calls[3].handle, third.handle);
    EXPECT_EQ(calls[4].handle, second.handle);
    EXPECT_EQ(calls[5].handle, bo.handle);
    for (size_t i = 3; i < calls.size(); ++i)
        EXPECT_FALSE(calls[i].acquire);
    EXPECT_EQ(unpins, (std::vector<size_t>{6}));
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(unpins.size(), 1u);
}

// A refused release leaves nothing to come back for the arena: every range is still released, then the arena once
TEST_F(GM20BHostAccess, AFailedMultipartReleaseStillReleasesEveryRangeAndTheArena)
{
    gm20b::Bo second = bo, third = bo;
    second.handle = 8;
    third.handle = 9;
    std::vector<size_t> unpins;
    gm20b::HostAccess access;
    ASSERT_TRUE(access.acquire(kernel, {{bo, 1, 1}, {second, 2, 1}, {third, 3, 1}}, false, &unpins, 9,
                               [](void *owner, uint32_t arena) {
                                   EXPECT_EQ(arena, 9u);
                                   static_cast<std::vector<size_t> *>(owner)->push_back(calls.size());
                               }));
    releaseFails = true;
    EXPECT_FALSE(access.finish());
    ASSERT_EQ(calls.size(), 6u);
    EXPECT_EQ(calls[3].handle, third.handle);
    EXPECT_EQ(calls[4].handle, second.handle);
    EXPECT_EQ(calls[5].handle, bo.handle);
    EXPECT_EQ(unpins, (std::vector<size_t>{6}));
    releaseFails = false;
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(calls.size(), 6u);
    EXPECT_EQ(unpins.size(), 1u);
}

TEST_F(GM20BHostAccess, TheDestructorReleasesTheArenaAfterAFailedMultipartRelease)
{
    std::vector<size_t> unpins;
    {
        gm20b::HostAccess access;
        gm20b::Bo second = bo;
        second.handle = 8;
        ASSERT_TRUE(access.acquire(kernel, {{bo, 1, 1}, {second, 2, 1}}, true, &unpins, 9, [](void *owner, uint32_t) {
            static_cast<std::vector<size_t> *>(owner)->push_back(calls.size());
        }));
        releaseFails = true;
    }
    EXPECT_EQ(reports, 1u);
    EXPECT_EQ(unpins, (std::vector<size_t>{4}));
}

TEST_F(GM20BHostAccess, AFailedUnwindStillReleasesTheArena)
{
    gm20b::Bo second = bo;
    second.handle = 8;
    refusedAcquireHandle = second.handle;
    releaseFails = true;
    std::vector<size_t> unpins;
    gm20b::HostAccess access;
    EXPECT_FALSE(access.acquire(kernel, {{bo, 1, 1}, {second, 2, 1}}, true, &unpins, 9, [](void *owner, uint32_t) {
        static_cast<std::vector<size_t> *>(owner)->push_back(calls.size());
    }));
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_FALSE(calls[2].acquire);
    EXPECT_EQ(unpins, (std::vector<size_t>{3}));
    releaseFails = false;
    EXPECT_TRUE(access.finish());
    EXPECT_EQ(unpins.size(), 1u);
}
