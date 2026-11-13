#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

#include "vm/switch_timebase_21H461.h"
#include "vm/switch_ui_scale_sites.h"

namespace {
constexpr uint64_t kChunkFileoff = SWITCH_TB_PAGE_FILEOFF + SWITCH_TB_CHUNK_OFFSET;

std::array<uint8_t, SWITCH_TB_CHUNK_SIZE> StockChunk()
{
    std::array<uint8_t, SWITCH_TB_CHUNK_SIZE> chunk;
    chunk.fill(0x5a);
    std::memcpy(chunk.data() + SWITCH_TB_INSN_OFFSET, switch_tb_stock, sizeof(switch_tb_stock));
    return chunk;
}
}  // namespace

TEST(XnuTimebase21H461, MappingRequiresThePinnedFileRangeAndAnAvailablePagerSlot)
{
    EXPECT_TRUE(switch_tb_mapping(0x18da98000, 0x6de4000, 0));
    EXPECT_TRUE(switch_tb_mapping(SWITCH_TB_PAGE_VMADDR, SWITCH_TB_PAGE_SIZE, SWITCH_TB_PAGE_FILEOFF));
    EXPECT_FALSE(switch_tb_mapping(0x18da98000, 0x6de4000, 1));
    EXPECT_FALSE(switch_tb_mapping(SWITCH_TB_PAGE_VMADDR + 1, SWITCH_TB_PAGE_SIZE, SWITCH_TB_PAGE_FILEOFF));
    EXPECT_FALSE(switch_tb_mapping(SWITCH_TB_PAGE_VMADDR, SWITCH_TB_PAGE_SIZE - 1, SWITCH_TB_PAGE_FILEOFF));
    EXPECT_FALSE(switch_tb_mapping(0, UINT64_MAX, UINT64_MAX));
    EXPECT_EQ(switch_us_site_for_slot(&switch_us_cache_21H461, SWITCH_TB_PAGER_SLOT, 0x18da98000, 0x6de4000, 0),
              nullptr);
    for (size_t i = 0; i < switch_us_cache_21H461.count; ++i) {
        EXPECT_NE(switch_us_cache_21H461.sites[i].page_vmaddr, SWITCH_TB_PAGE_VMADDR);
    }
}

TEST(XnuTimebase21H461, ExactCacheIdentityIsRequired)
{
    std::array<uint8_t, SWITCH_TB_UUID_OFFSET + 16> header{};
    std::memcpy(header.data(), switch_tb_magic, sizeof(switch_tb_magic));
    std::memcpy(header.data() + SWITCH_TB_UUID_OFFSET, switch_tb_uuid, sizeof(switch_tb_uuid));
    ASSERT_TRUE(switch_tb_header(header.data(), header.size()));
    EXPECT_FALSE(switch_tb_header(header.data(), header.size() - 1));
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned offset = i < 16 ? i : SWITCH_TB_UUID_OFFSET + i - 16;
        header[offset] ^= 1;
        EXPECT_FALSE(switch_tb_header(header.data(), header.size()));
        header[offset] ^= 1;
    }
}

TEST(XnuTimebase21H461, OnlyTheConversionChangesAndAnyGuardMismatchLeavesTheWholeChunkUntouched)
{
    auto chunk = StockChunk();
    auto expected = chunk;
    std::memcpy(expected.data() + SWITCH_TB_INSN_OFFSET, switch_tb_native, sizeof(switch_tb_native));
    ASSERT_TRUE(switch_tb_patch(chunk.data(), chunk.size(), kChunkFileoff));
    EXPECT_EQ(chunk, expected);
    for (size_t i = 0; i < sizeof(switch_tb_stock); ++i) {
        chunk = StockChunk();
        chunk[SWITCH_TB_INSN_OFFSET + i] ^= 1;
        const auto before = chunk;
        EXPECT_FALSE(switch_tb_patch(chunk.data(), chunk.size(), kChunkFileoff));
        EXPECT_EQ(chunk, before);
    }
    chunk = StockChunk();
    EXPECT_FALSE(switch_tb_patch(chunk.data(), chunk.size() - 1, kChunkFileoff));
    EXPECT_FALSE(switch_tb_patch(chunk.data(), chunk.size(), kChunkFileoff + 1));
    EXPECT_EQ(chunk, StockChunk());
}

TEST(XnuTimebase21H461, ActualReplacementInstructionsMatchWideArithmeticAtBoundariesAndRandomUptimes)
{
    constexpr size_t pageSize = 0x4000;
    auto *code = static_cast<uint32_t *>(
        mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
    ASSERT_NE(code, MAP_FAILED);
    pthread_jit_write_protect_np(0);
    std::memset(code, 0, pageSize);
    code[0x100 / 4] = 0xaa0003e9;  // mov x9,x0
    code[0x104 / 4] = 0xaa0103e8;  // mov x8,x1
    code[0x108 / 4] = 0x1400013c;  // b 0x5f8
    std::memcpy(code + SWITCH_TB_INSN_OFFSET / 4, switch_tb_native, sizeof(switch_tb_native));
    code[0x650 / 4] = 0xaa0903e0;  // mov x0,x9
    code[0x654 / 4] = 0xf9000048;  // str x8,[x2]
    code[0x658 / 4] = 0xf9000083;  // str x3,[x4]
    code[0x65c / 4] = 0xd65f03c0;  // ret
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, pageSize);
    const auto convert =
        reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t *, uint64_t, uint64_t *)>(code + 0x100 / 4);
    auto check = [&](uint64_t nanoseconds) {
        const auto expected = static_cast<uint64_t>(static_cast<__uint128_t>(nanoseconds) * 12 / 625);
        uint64_t leeway = 0, queue = 0;
        EXPECT_EQ(convert(nanoseconds, 1'000'000'000, &leeway, 0xcafe12345678, &queue), expected)
            << "nanoseconds=" << nanoseconds;
        EXPECT_EQ(leeway, 1'000'000'000u);
        EXPECT_EQ(queue, 0xcafe12345678u);
    };
    for (uint64_t ns = 0; ns < 12500; ++ns) {
        check(ns);
    }
    for (uint64_t ns : {UINT64_C(60'050'000'000), UINT64_C(3'600'050'000'000), UINT64_MAX / 3, UINT64_MAX / 3 + 1,
                        UINT64_MAX / 2 - 1, UINT64_MAX / 2, UINT64_MAX}) {
        check(ns);
    }
    std::mt19937_64 random(0x19200000);
    for (unsigned i = 0; i < 100000; ++i) {
        check(random());
    }
    EXPECT_EQ(munmap(code, pageSize), 0);
}

TEST(XnuTimebase21H461, OnlyTheNativeRatioAndReservedSlotEnableThePager)
{
    EXPECT_TRUE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 625, 12));
    EXPECT_TRUE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 1250, 24));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 125, 3));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 1, 1));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 0, 0));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 0, 12));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, 625, 0));
    EXPECT_FALSE(switch_tb_eligible(SWITCH_TB_PAGER_SLOT, UINT32_MAX, UINT32_MAX));
    EXPECT_FALSE(switch_tb_eligible(0, 625, 12));
    EXPECT_FALSE(switch_tb_eligible(2, 625, 12));
}
