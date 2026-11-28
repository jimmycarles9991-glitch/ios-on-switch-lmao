// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bootstrap/threads/memory.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

constexpr uint64_t kPage = 16384;
constexpr uint32_t kOwnedTag = 0xffffffffu;

switch_vm_region Region(uint32_t tag, uint32_t resident, uint32_t dirty, uint32_t swapped, uint64_t size)
{
    return switch_vm_region{tag, resident, dirty, swapped, size};
}

std::string Format(const switch_vm_snapshot &snap, const switch_vm_tag_row *rows, size_t count,
                   const switch_vm_tag_row &total, size_t limit)
{
    char *buffer = nullptr;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);
    switch_vm_tags_format(stream, &snap, rows, count, &total, limit);
    fclose(stream);
    std::string text(buffer, length);
    free(buffer);
    return text;
}

TEST(SwitchMemoryTest, SumsByTagAndRanksByFootprint)
{
    const switch_vm_region regions[] = {
        Region(1, 10, 8, 2, 64 * kPage),  Region(21, 300, 300, 0, 300 * kPage),
        Region(1, 5, 5, 1, 16 * kPage),   Region(11, 40, 30, 60, 32768 * kPage),
        Region(0, 0, 0, 0, 1024 * kPage), Region(kOwnedTag, 4, 4, 1, 5 * kPage),
    };
    switch_vm_tag_row rows[SWITCH_VM_TAG_SLOTS];
    switch_vm_tag_row total;
    const size_t kept = switch_vm_tags_sum(regions, 6, kPage, rows, &total);

    ASSERT_EQ(kept, 4u);
    EXPECT_EQ(rows[0].tag, 21u);
    EXPECT_EQ(rows[1].tag, 11u);
    EXPECT_EQ(rows[2].tag, 1u);
    EXPECT_EQ(rows[2].regions, 2u);
    EXPECT_EQ(rows[2].dirty_bytes, 13 * kPage);
    EXPECT_EQ(rows[2].swapped_bytes, 3 * kPage);
    EXPECT_EQ(rows[3].tag, SWITCH_VM_TAG_OWNED);
    EXPECT_EQ(total.regions, 6u);
    EXPECT_EQ(total.dirty_bytes, (8 + 300 + 5 + 30 + 4) * kPage);
    EXPECT_EQ(total.swapped_bytes, (2 + 1 + 60 + 1) * kPage);
    EXPECT_EQ(total.virtual_bytes, (64 + 300 + 16 + 32768 + 1024 + 5) * kPage);
}

TEST(SwitchMemoryTest, NamesTheKernelsTagsAndTheOwnedRegion)
{
    EXPECT_STREQ(switch_vm_tag_name(1), "MALLOC");
    EXPECT_STREQ(switch_vm_tag_name(21), "IOKIT");
    EXPECT_STREQ(switch_vm_tag_name(100), "IOACCELERATOR");
    EXPECT_STREQ(switch_vm_tag_name(SWITCH_VM_TAG_OWNED), "owned (ledger)");
    EXPECT_STREQ(switch_vm_tag_name(247), "APPLICATION_SPECIFIC");
    EXPECT_STREQ(switch_vm_tag_name(200), "-");
}

TEST(SwitchMemoryTest, PrintsTheFootprintBesideTheWalkedTotal)
{
    const switch_vm_region regions[] = {Region(1, 64, 64, 0, 64 * kPage), Region(kOwnedTag, 2, 2, 0, 2 * kPage)};
    switch_vm_tag_row rows[SWITCH_VM_TAG_SLOTS];
    switch_vm_tag_row total;
    const size_t kept = switch_vm_tags_sum(regions, 2, kPage, rows, &total);
    const switch_vm_snapshot snap = {"iconservicesagent", 300, 42.5, 1072 * 1024, 2048 * 1024};

    const std::string text = Format(snap, rows, kept, total, 1);

    EXPECT_NE(text.find("t=42.5s iconservicesagent [300] footprint 1072 KB, lifetime peak 2048 KB; regions dirty "
                        "1056 KB + compressed 0 KB = 1056 KB; not in a region 16 KB\n"),
              std::string::npos);
    EXPECT_NE(text.find("1    MALLOC "), std::string::npos);
    EXPECT_EQ(text.find("owned (ledger)"), std::string::npos);
}

}  // namespace
