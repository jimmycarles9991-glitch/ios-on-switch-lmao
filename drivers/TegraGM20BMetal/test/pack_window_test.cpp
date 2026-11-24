// SPDX-License-Identifier: MIT
#include "../pack_window.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

namespace {

gm20b::PackHeader validHeader()
{
    gm20b::PackHeader h = {};
    std::memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.stack = 0xfeedfacecafebeefull;
    h.section[gm20b::PackHeap] = {gm20b::kPackTierAlign, 4 * gm20b::kPackTierAlign};
    h.tier0[gm20b::PackHeap] = 2 * gm20b::kPackTierAlign;
    return h;
}

TEST(GM20BPackWindow, EligibilityRejectsMalformedOrEmptyTier0)
{
    const gm20b::PackHeader good = validHeader();
    EXPECT_TRUE(gm20b::packWindowEligible(good, TGM_PACK_WINDOW_BYTES));

    {
        gm20b::PackHeader h = good;
        h.version = gm20b::kPackVersion - 1;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
    {
        gm20b::PackHeader h = good;
        h.tier0[gm20b::PackHeap] = 0;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
    {
        gm20b::PackHeader h = good;
        h.section[gm20b::PackHeap].offset += 4096;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
    {
        gm20b::PackHeader h = good;
        h.tier0[gm20b::PackHeap] += 4096;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
    {
        gm20b::PackHeader h = good;
        h.tier0[gm20b::PackHeap] = h.section[gm20b::PackHeap].bytes + gm20b::kPackTierAlign;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
    {
        gm20b::PackHeader h = good;
        h.section[gm20b::PackHeap].bytes = TGM_PACK_WINDOW_BYTES + gm20b::kPackTierAlign;
        EXPECT_FALSE(gm20b::packWindowEligible(h, TGM_PACK_WINDOW_BYTES));
    }
}

TEST(GM20BPackWindow, ReplyMatchingValidatesStatusVaStackAndSizes)
{
    const gm20b::PackHeader h = validHeader();
    TegraGM20BAbiPackImageMapReply r = {};
    r.status = TegraGM20BPackStatusReady;
    r.gpu_va = TGM_PACK_WINDOW_VA;
    r.stack = h.stack;
    r.tier0_heap_bytes = h.tier0[gm20b::PackHeap];
    r.heap_bytes = h.section[gm20b::PackHeap].bytes;
    EXPECT_TRUE(gm20b::packWindowMatchesReply(h, r, TGM_PACK_WINDOW_VA, TGM_PACK_WINDOW_BYTES));

    auto bad = [&](auto mutate) {
        TegraGM20BAbiPackImageMapReply copy = r;
        mutate(copy);
        EXPECT_FALSE(gm20b::packWindowMatchesReply(h, copy, TGM_PACK_WINDOW_VA, TGM_PACK_WINDOW_BYTES));
    };
    bad([](auto &c) {
        c.status = TegraGM20BPackStatusRefused;
    });
    bad([](auto &c) {
        c.gpu_va = TGM_PACK_WINDOW_VA + 0x10000;
    });
    bad([](auto &c) {
        c.stack ^= 1ull;
    });
    bad([](auto &c) {
        c.tier0_heap_bytes = 0;
    });
    bad([](auto &c) {
        c.tier0_heap_bytes -= gm20b::kProgramGrain;
    });
    bad([](auto &c) {
        c.heap_bytes += gm20b::kPackTierAlign;
    });
}

TEST(GM20BPackWindow, ProgramContainmentChecksPrefetchBoundary)
{
    const uint64_t windowBytes = 2 * gm20b::kPackTierAlign;
    gm20b::PackProgram p = {};
    p.heapOffset = 0;
    p.bytes = 0x200;
    EXPECT_FALSE(gm20b::packWindowContainsProgram(p, 0));
    EXPECT_TRUE(gm20b::packWindowContainsProgram(p, windowBytes));

    p.heapOffset = uint32_t(windowBytes - gm20b::kProgramPrefetchBytes - 0x200);
    EXPECT_TRUE(gm20b::packWindowContainsProgram(p, windowBytes));

    p.heapOffset = uint32_t(windowBytes - gm20b::kProgramPrefetchBytes - 0x1ff);
    EXPECT_FALSE(gm20b::packWindowContainsProgram(p, windowBytes));

    p.heapOffset = UINT32_MAX;
    EXPECT_FALSE(gm20b::packWindowContainsProgram(p, windowBytes));

    p.heapOffset = 0;
    p.bytes = 0;
    EXPECT_FALSE(gm20b::packWindowContainsProgram(p, windowBytes));
}

TEST(GM20BPackWindow, CollectCpuRangesCoalescesAlignsAndEnforcesCaps)
{
    constexpr uint64_t kBase = 0x100000000ull;
    const auto *base = reinterpret_cast<const uint8_t *>(uintptr_t(kBase));
    gm20b::PackHeader h = validHeader();
    h.section[gm20b::PackFastKeys] = {0x1200, 0x800};
    h.tier0[gm20b::PackFastKeys] = 0x800;
    h.section[gm20b::PackPipelines] = {0x2400, 0x3000};
    h.tier0[gm20b::PackPipelines] = 0x1800;
    h.section[gm20b::PackPrograms] = {0x10000, 0x2000};
    h.tier0[gm20b::PackPrograms] = 0x1000;
    h.section[gm20b::PackHeap] = {0x40000, 0x80000};
    h.tier0[gm20b::PackHeap] = 0x40000;

    TegraGM20BAbiPackCpuRange out[gm20b::kPackWindowMaxCpuRanges] = {};
    const uint32_t n16 = gm20b::packWindowCollectCpuRanges(base, h, 16384, out);
    ASSERT_EQ(n16, 2u);
    EXPECT_EQ(out[0].address, kBase + 0x0);
    EXPECT_EQ(out[0].bytes, 16384u);
    EXPECT_EQ(out[1].address, kBase + 0x10000);
    EXPECT_EQ(out[1].bytes, 16384u);

    const uint32_t n4 = gm20b::packWindowCollectCpuRanges(base, h, 4096, out);
    ASSERT_EQ(n4, 2u);
    EXPECT_EQ(out[0].address, kBase + 0x1000);
    EXPECT_EQ(out[0].bytes, 3 * 4096u);
    EXPECT_EQ(out[1].address, kBase + 0x10000);
    EXPECT_EQ(out[1].bytes, 4096u);

    gm20b::PackHeader many = {};
    many.version = gm20b::kPackVersion;
    const gm20b::PackSectionId ids[10] = {
        gm20b::PackFastKeys, gm20b::PackPipelines, gm20b::PackPrograms, gm20b::PackVariants, gm20b::PackBlob,
        gm20b::PackStrings,  gm20b::PackSplits,    gm20b::PackIterated, gm20b::PackInit,     gm20b::PackBaseline,
    };
    for (uint32_t i = 0; i < 10; ++i) {
        const uint64_t off = (uint64_t(i) + 1) * 0x10000;
        many.section[ids[i]] = {off, 4096};
        many.tier0[ids[i]] = 4096;
    }
    const uint32_t capped = gm20b::packWindowCollectCpuRanges(base, many, 16384, out);
    ASSERT_EQ(capped, gm20b::kPackWindowMaxCpuRanges);
    for (uint32_t i = 0; i < capped; ++i)
        EXPECT_EQ(out[i].address, kBase + (uint64_t(i) + 1) * 0x10000);

    gm20b::PackHeader huge = {};
    huge.version = gm20b::kPackVersion;
    huge.section[gm20b::PackFastKeys] = {0x10000, 16384};
    huge.tier0[gm20b::PackFastKeys] = 16384;
    huge.section[gm20b::PackPipelines] = {0x100000, 70ULL << 20};
    huge.tier0[gm20b::PackPipelines] = 70ULL << 20;
    huge.section[gm20b::PackPrograms] = {0x5000000, 32768};
    huge.tier0[gm20b::PackPrograms] = 32768;
    const uint32_t budgetCount = gm20b::packWindowCollectCpuRanges(base, huge, 16384, out);
    ASSERT_EQ(budgetCount, 2u);
    EXPECT_EQ(out[0].address, kBase + 0x10000);
    EXPECT_EQ(out[0].bytes, 16384u);
    EXPECT_EQ(out[1].address, kBase + 0x5000000);
    EXPECT_EQ(out[1].bytes, 32768u);

    EXPECT_EQ(gm20b::packWindowCollectCpuRanges(base + 4096, h, 16384, out), 0u);
    EXPECT_EQ(gm20b::packWindowCollectCpuRanges(base, h, 12345, out), 0u);
    EXPECT_EQ(gm20b::packWindowCollectCpuRanges(nullptr, h, 16384, out), 0u);
}

}  // namespace
