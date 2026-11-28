// SPDX-License-Identifier: MIT
// The build's own pack (//gpu/seed:pack) as the render server wires it at open (pack_window.h
// packWindowCollectCpuRanges): within the ranges the kernel takes, every table a creation reads, all of the fast keys
// and the segments' words, so no creation or draw of a tier-0 state reads a page from the card.
#include "../pack.h"
#include "../pack_window.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <string>

namespace {

std::string gPackPath;

constexpr uint64_t kDevicePage = 16 << 10;  // arm64 iOS's vm_page_size

TEST(RealPackWindow, TheRenderServerWiresEveryTableACreationReads)
{
    ASSERT_FALSE(gPackPath.empty());
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(gPackPath.c_str(), 0, err)) << err;
    const gm20b::PackHeader &h = *pack.header();
    const auto *base = reinterpret_cast<const uint8_t *>(pack.header());
    ASSERT_EQ(uintptr_t(base) % kDevicePage, 0u);
    TegraGM20BAbiPackCpuRange ranges[gm20b::kPackWindowMaxCpuRanges] = {};
    const uint32_t count = gm20b::packWindowCollectCpuRanges(base, h, kDevicePage, ranges);
    ASSERT_GT(count, 0u);
    uint64_t wired = 0;
    for (uint32_t i = 0; i < count; i++)
        wired += ranges[i].bytes;
    EXPECT_LE(wired, gm20b::kPackWindowMaxCpuWireBytes);
    const auto covered = [&](uint64_t offset, uint64_t bytes) {
        const uint64_t from = uint64_t(uintptr_t(base)) + offset;
        for (uint32_t i = 0; i < count; i++)
            if (ranges[i].address <= from && from + bytes <= ranges[i].address + ranges[i].bytes)
                return true;
        return bytes == 0;
    };
    EXPECT_TRUE(covered(h.section[gm20b::PackFastKeys].offset, h.section[gm20b::PackFastKeys].bytes)) << "fast keys";
    for (const gm20b::PackSectionId id :
         {gm20b::PackPipelines, gm20b::PackPrograms, gm20b::PackSegments, gm20b::PackVariants, gm20b::PackBlob,
          gm20b::PackStrings, gm20b::PackSplits, gm20b::PackIterated, gm20b::PackInit, gm20b::PackBaseline,
          gm20b::PackSamplers})
        EXPECT_TRUE(covered(h.section[id].offset, std::min(h.tier0[id], h.section[id].bytes))) << "section " << id;
    uint32_t wordsAt = 0, wordsBytes = 0;
    ASSERT_TRUE(pack.segmentWords(wordsAt, wordsBytes));
    EXPECT_TRUE(covered(h.section[gm20b::PackHeap].offset + wordsAt, wordsBytes)) << "segment words";
    printf("wired %llu KiB in %u ranges; segment words %u KiB; tier 0 heap %llu KiB\n",
           (unsigned long long)(wired >> 10), count, wordsBytes >> 10,
           (unsigned long long)(h.tier0[gm20b::PackHeap] >> 10));
}

}  // namespace

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    if (argc > 1)
        gPackPath = argv[1];
    return RUN_ALL_TESTS();
}
